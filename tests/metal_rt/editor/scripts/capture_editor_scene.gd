@tool
extends Node3D

const FIXTURE_REVISION := "e0-hg0-v1"
const CAPTURE_SIZE := Vector2i(64, 64)
const BEAUTY_FRAME := 120
const INSTANCE_ID_FRAME := 150

@onready var camera: Camera3D = $Camera
@onready var environment: Environment = $WorldEnvironment.environment

var capture_enabled := false
var capture_frame := 0
var artifact_dir := ""
var capture_label := "cold"
var capture_viewport: Viewport
var capture_camera: Camera3D


func _ready() -> void:
	camera.look_at_from_position(Vector3(5.2, 3.8, 6.8), Vector3(0.0, 0.65, 0.0))
	camera.make_current()
	capture_viewport = get_viewport()
	capture_camera = camera
	if Engine.is_editor_hint():
		capture_viewport = EditorInterface.get_editor_viewport_3d(0)
		capture_camera = capture_viewport.get_camera_3d()
	_configure_capture_camera()
	capture_enabled = OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
	artifact_dir = OS.get_environment("GODOT_MRT_ARTIFACT_DIR")
	capture_label = OS.get_environment("GODOT_MRT_CAPTURE_LABEL")
	if capture_label.is_empty():
		capture_label = "cold"
	set_process(capture_enabled and not artifact_dir.is_empty())


func _process(_delta: float) -> void:
	if not capture_enabled:
		return
	_configure_capture_camera()
	capture_frame += 1
	if capture_frame == BEAUTY_FRAME:
		_capture("beauty")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_INSTANCE_ID
	elif capture_frame == INSTANCE_ID_FRAME:
		_capture("instance_id")
		_write_manifest()
		print("METAL_RT_FIXTURE=e0_hg0")
		print("METAL_RT_FIXTURE_REVISION=%s" % FIXTURE_REVISION)
		print("METAL_RT_CAPTURE_LABEL=%s" % capture_label)
		get_tree().quit()


func _capture(kind: String) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := capture_viewport.get_texture().get_image()
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var path := artifact_dir.path_join("e0_hg0_%s_%s.png" % [capture_label, kind])
	var error := captured.save_png(path)
	if error != OK:
		push_error("Failed to save C13 editor capture %s: %s" % [path, error_string(error)])


func _configure_capture_camera() -> void:
	if capture_camera != null:
		capture_camera.fov = 48.0
		capture_camera.look_at_from_position(Vector3(5.2, 3.8, 6.8), Vector3(0.0, 0.65, 0.0))


func _write_manifest() -> void:
	var manifest := {
		"schema_version": 1,
		"fixture": "e0_hg0",
		"fixture_revision": FIXTURE_REVISION,
		"capture_label": capture_label,
		"renderer": "forward_plus",
		"rendering_driver": "metal",
		"resolution": [CAPTURE_SIZE.x, CAPTURE_SIZE.y],
		"color_space": "viewport_srgb",
		"format": "rgba8_png",
		"rng_seed_contract": "pixel_frame_sample_pcg",
		"samples_per_pixel": 4,
		"max_bounces": 2,
		"denoiser": "none",
		"upscaler": "none",
		"warmup_frames": BEAUTY_FRAME,
		"beauty_capture_frame": BEAUTY_FRAME,
		"instance_id_capture_frame": INSTANCE_ID_FRAME,
		"camera_position": [5.2, 3.8, 6.8],
		"camera_target": [0.0, 0.65, 0.0],
		"light_subset": "one directional light",
		"environment_subset": "procedural sky, no fog",
		"material_subset": "opaque StandardMaterial3D albedo/color, albedo texture, normal/ORM inputs, emission, metallic, roughness",
		"geometry_subset": "static triangle meshes",
		"device": RenderingServer.get_video_adapter_name(),
		"device_vendor": RenderingServer.get_video_adapter_vendor(),
		"os": OS.get_name(),
		"os_version": OS.get_version(),
		"model": OS.get_model_name(),
		"architecture": Engine.get_architecture_name(),
		"editor_process": Engine.is_editor_hint(),
		"beauty_sha256": FileAccess.get_sha256(artifact_dir.path_join("e0_hg0_%s_beauty.png" % capture_label)),
		"instance_id_sha256": FileAccess.get_sha256(artifact_dir.path_join("e0_hg0_%s_instance_id.png" % capture_label)),
		"unsupported": [
			"alpha testing and transparent materials",
			"custom spatial shaders",
			"skinned, blend-shape, and deformed meshes",
			"MultiMesh",
			"procedural AABBs",
			"fog and debug modes other than instance ID",
			"native denoising and DLSS Ray Reconstruction",
			"shader execution reordering",
			"export templates",
		],
	}
	var manifest_path := artifact_dir.path_join("e0_hg0_%s_manifest.json" % capture_label)
	var file := FileAccess.open(manifest_path, FileAccess.WRITE)
	if file == null:
		push_error("Failed to open C13 manifest: %s" % manifest_path)
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
