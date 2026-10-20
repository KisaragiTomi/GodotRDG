def can_build(env, platform):
    # RDG sits on top of RenderingDevice, which needs at least one RD driver.
    return env.get("vulkan", False) or env.get("d3d12", False) or env.get("metal", False)


def configure(env):
    pass


def get_doc_classes():
    return [
        "RenderGraphTests",
        "RDGCompositorEffect",
    ]


def get_doc_path():
    return "doc_classes"
