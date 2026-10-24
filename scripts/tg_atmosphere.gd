class_name TGAtmosphere
extends Node3D
## TG density/source equations on Godot's native shadowed volumetric integrator.
## The reference's six jittered samples and two fake lights are not reproduced.

@export_range(0.0, 4.0) var thickness := 1.0
@export_range(0.01, 2.0) var distance_scale := 0.2
@export_range(0.0, 4.0) var density_scale := 1.0
@export_range(-0.8, 0.8) var anisotropy := 0.65
@export_range(0.0, 16.0) var sunlight := 4.0
@export_range(0.0, 4.0) var sky_energy := 1.0
@export var sky_enabled := true
@export var sky_color := Color(0.24, 0.32, 0.45)
@export var tint := Color(0.94, 0.97, 1.0)
@export var center := Vector3.ZERO
@export_range(0, 4) var theme := 0
@export var fog_length := 55.0
@export_range(0.5, 6.0) var detail_spread := 2.0
@export var active := true
# Native Halton sub-cell jitter is gated on reprojection being enabled.
@export var temporal_reprojection := true

var environment: Environment
var sun: DirectionalLight3D
var volume: FogVolume
var material: ShaderMaterial
var _last_sun_transform := Transform3D()
var _last_camera_transform := Transform3D()
var _last_sun_energy := -1.0
var _last_sun_visible := true
var _last_camera_projection := Vector3.ZERO
var _history_cooldown := 3
var _saved_environment: Dictionary = {}
var _saved_sun_energy := 1.0

func setup(target_environment: Environment, directional_light: DirectionalLight3D) -> void:
	environment = target_environment
	sun = directional_light
	for property in ["volumetric_fog_enabled", "volumetric_fog_density", "volumetric_fog_anisotropy", "volumetric_fog_length", "volumetric_fog_detail_spread", "volumetric_fog_temporal_reprojection_enabled", "volumetric_fog_temporal_reprojection_amount", "volumetric_fog_ambient_inject", "volumetric_fog_sky_affect"]:
		_saved_environment[property] = environment.get(property)
	_saved_sun_energy = sun.light_volumetric_fog_energy
	volume = FogVolume.new()
	volume.name = "TGAtmosphereVolume"
	volume.shape = RenderingServer.FOG_VOLUME_SHAPE_WORLD
	material = ShaderMaterial.new()
	material.shader = preload("res://shaders/tg_atmosphere.gdshader")
	volume.material = material
	add_child(volume)
	apply()

func invalidate_history() -> void:
	_history_cooldown = 2
	# Clear stale lighting without turning off sub-cell antialiasing.
	if environment != null: environment.volumetric_fog_temporal_reprojection_amount = 0.0

func sky_radiance() -> Vector3:
	# Shared unattenuated source. Fog thickness/visibility must not change GI.
	var linear := sky_color.srgb_to_linear()
	return Vector3(linear.r, linear.g, linear.b) * maxf(sky_energy, 0.0) if sky_enabled else Vector3.ZERO

func apply() -> void:
	if environment == null: return
	environment.volumetric_fog_enabled = active and thickness > 0.0 and density_scale > 0.0
	environment.volumetric_fog_density = 0.0
	environment.volumetric_fog_anisotropy = clampf(anisotropy, -0.8, 0.8)
	environment.volumetric_fog_length = fog_length
	environment.volumetric_fog_detail_spread = detail_spread
	environment.volumetric_fog_temporal_reprojection_enabled = temporal_reprojection
	environment.volumetric_fog_temporal_reprojection_amount = 0.9
	environment.volumetric_fog_ambient_inject = 0.0
	environment.volumetric_fog_sky_affect = 1.0
	volume.visible = environment.volumetric_fog_enabled
	material.set_shader_parameter("distance_scale", distance_scale)
	material.set_shader_parameter("extinction", 0.0125 * density_scale * thickness * 1.2 / maxf(distance_scale, 0.001))
	material.set_shader_parameter("center", center)
	material.set_shader_parameter("tint", tint)
	material.set_shader_parameter("sky_color", sky_color)
	material.set_shader_parameter("sky_energy", maxf(sky_energy, 0.0) if sky_enabled else 0.0)
	material.set_shader_parameter("theme", theme)
	_update_sun()
	invalidate_history()

func _update_sun() -> void:
	# Preserve the reference's incoming sunlight attenuation under scene scaling.
	# This does not apply its additional constant camera-transmittance multiplier.
	var elevation := maxf(0.3, sun.global_basis.z.normalized().y)
	var attenuation := exp(-0.0125 * density_scale * thickness * 36.0 / elevation)
	sun.light_volumetric_fog_energy = sunlight * attenuation if active else _saved_sun_energy
	material.set_shader_parameter("solar_transmittance", attenuation)

func _process(_delta: float) -> void:
	if environment == null or not is_instance_valid(sun): return
	var camera := get_viewport().get_camera_3d()
	if not sun.global_transform.is_equal_approx(_last_sun_transform) or sun.light_energy != _last_sun_energy or sun.is_visible_in_tree() != _last_sun_visible:
		_last_sun_transform = sun.global_transform
		_last_sun_energy = sun.light_energy
		_last_sun_visible = sun.is_visible_in_tree()
		_update_sun()
		invalidate_history()
	# Ordinary motion has valid world-space reprojection. Reset only for cuts;
	# resetting on every transform change exposes the coarse grid while flying.
	if camera != null:
		var displacement := camera.global_position.distance_to(_last_camera_transform.origin)
		var facing := camera.global_basis.z.dot(_last_camera_transform.basis.z)
		var projection := Vector3(camera.fov, camera.size, camera.projection)
		if displacement > 3.0 or facing < 0.7 or projection != _last_camera_projection: invalidate_history()
		_last_camera_transform = camera.global_transform
		_last_camera_projection = projection
	environment.volumetric_fog_temporal_reprojection_enabled = temporal_reprojection
	environment.volumetric_fog_temporal_reprojection_amount = 0.0 if _history_cooldown > 0 else 0.9
	_history_cooldown = maxi(_history_cooldown - 1, 0)

func _exit_tree() -> void:
	if is_instance_valid(environment):
		for property in _saved_environment: environment.set(property, _saved_environment[property])
	if is_instance_valid(sun): sun.light_volumetric_fog_energy = _saved_sun_energy
