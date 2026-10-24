def can_build(env, platform):
    # Built on the rdg module (UE-style render graph on RenderingDevice).
    env.module_add_dependencies("tg_gi", ["rdg"])
    return env.get("vulkan", False) or env.get("d3d12", False) or env.get("metal", False)


def configure(env):
    pass


def get_doc_classes():
    return [
        "TGScreenGI",
        "TGScreenGIHistory",
        "TGSurfaceFog",
    ]


def get_doc_path():
    return "doc_classes"
