@tool
extends Node3D

const FIXTURE_REVISION := "e0-hg0-v1"
const CAPTURE_SIZE := Vector2i(64, 64)
const BEAUTY_FRAME := 120
const INSTANCE_ID_FRAME := 150
const BOX_RESIZE_START_FRAME := 10
const BOX_RESIZE_ITERATIONS := 20
const PRESENTATION_MODE_WARMUP_FRAMES := 75
const PRESENTATION_TRANSITION_WARMUP_FRAMES := 60

@onready var camera: Camera3D = $Camera
@onready var environment: Environment = $WorldEnvironment.environment
@onready var box_mesh: BoxMesh = $Box.mesh

var capture_enabled := false
var capture_frame := 0
var artifact_dir := ""
var capture_label := "cold"
var capture_viewport: Viewport
var capture_camera: Camera3D
var original_box_size := Vector3.ZERO
var presentation_enabled := false
var presentation_phase := ""
var presentation_phase_frame := 0
var presentation_mode_index := 0
var presentation_modes: Array[Dictionary] = []
var presentation_captures: Dictionary = {}
var presentation_events: Array[String] = []
var presentation_temporal_mode: Dictionary = {}
var presentation_original_window_size := Vector2i.ZERO
var presentation_secondary_viewport: SubViewport


func _ready() -> void:
	if (
		Engine.is_editor_hint()
		and OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") in ["e1_geometry", "e2_materials", "e3_procedural"]
	):
		var fixture := OS.get_environment("GODOT_MRT_FIXTURE")
		EditorInterface.call_deferred("open_scene_from_path", "res://fixtures/%s.tscn" % fixture)
		return
	camera.look_at_from_position(Vector3(5.2, 3.8, 6.8), Vector3(0.0, 0.65, 0.0))
	camera.make_current()
	original_box_size = box_mesh.size
	capture_viewport = get_viewport()
	capture_camera = camera
	if Engine.is_editor_hint():
		capture_viewport = EditorInterface.get_editor_viewport_3d(0)
		capture_camera = capture_viewport.get_camera_3d()
	_configure_capture_camera()
	capture_enabled = (
		OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") == "e0_hg0"
	)
	artifact_dir = OS.get_environment("GODOT_MRT_ARTIFACT_DIR")
	capture_label = OS.get_environment("GODOT_MRT_CAPTURE_LABEL")
	if capture_label.is_empty():
		capture_label = "cold"
	presentation_enabled = OS.get_environment("GODOT_MRT_PRESENTATION_CAPTURE") == "1"
	if presentation_enabled:
		process_mode = Node.PROCESS_MODE_ALWAYS
		if not _validate_presentation_ux():
			return
		_build_presentation_modes()
		presentation_original_window_size = DisplayServer.window_get_size()
		presentation_phase = "matrix"
	set_process(capture_enabled and not artifact_dir.is_empty())


func _process(_delta: float) -> void:
	if not capture_enabled:
		return
	if presentation_enabled:
		_process_presentation()
		return
	_configure_capture_camera()
	capture_frame += 1
	if capture_frame >= BOX_RESIZE_START_FRAME and capture_frame < BOX_RESIZE_START_FRAME + BOX_RESIZE_ITERATIONS:
		var resize_step := capture_frame - BOX_RESIZE_START_FRAME + 1
		box_mesh.size = original_box_size + Vector3(0.01 * resize_step, 0.0, 0.0)
	elif capture_frame == BOX_RESIZE_START_FRAME + BOX_RESIZE_ITERATIONS:
		box_mesh.size = original_box_size
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


func _validate_presentation_ux() -> bool:
	var default_environment := Environment.new()
	if default_environment.pathtracing_denoiser != RenderingServer.PT_DENOISER_NONE:
		return _fail_presentation("A new Environment did not default to the None denoiser.")
	if RenderingServer.is_pathtracing_denoiser_supported(RenderingServer.PT_DENOISER_DLSS_RAY_RECONSTRUCTION):
		return _fail_presentation("Metal unexpectedly advertised DLSS Ray Reconstruction.")
	var metalfx_supported := RenderingServer.is_pathtracing_denoiser_supported(RenderingServer.PT_DENOISER_METALFX)

	var denoiser_hint := ""
	for property: Dictionary in default_environment.get_property_list():
		if property.get("name") == "pathtracing_denoiser":
			denoiser_hint = property.get("hint_string", "")
			break
	var expected_hint := "None:0"
	if metalfx_supported:
		expected_hint += ",MetalFX Denoised Upscaling:2"
	if denoiser_hint != expected_hint:
		return _fail_presentation("The Metal inspector denoiser hint was '%s', expected '%s'." % [denoiser_hint, expected_hint])

	var windows_environment: Environment = load("res://fixtures/presentation_windows_dlss_rr.tres")
	if windows_environment == null:
		return _fail_presentation("The cross-platform DLSS-RR Environment fixture did not load.")
	if windows_environment.pathtracing_denoiser != RenderingServer.PT_DENOISER_NONE:
		return _fail_presentation("The Windows DLSS-RR fixture did not fall back to None on Metal.")

	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var sanitized_path := artifact_dir.path_join("presentation_sanitized_environment.tres")
	var save_error := ResourceSaver.save(windows_environment, sanitized_path)
	if save_error != OK:
		return _fail_presentation("The sanitized Environment could not be serialized: %s" % error_string(save_error))
	var serialized := FileAccess.get_file_as_string(sanitized_path)
	if "pathtracing_denoiser = 1" in serialized:
		return _fail_presentation("The sanitized Environment serialized the unsupported DLSS-RR value.")
	return true


func _build_presentation_modes() -> void:
	presentation_modes = [
		{"name": "native", "mode": Viewport.SCALING_3D_MODE_BILINEAR, "scale": 1.0, "temporal": false},
		{"name": "fsr1", "mode": Viewport.SCALING_3D_MODE_FSR, "scale": 0.67, "temporal": false},
		{"name": "fsr2", "mode": Viewport.SCALING_3D_MODE_FSR2, "scale": 0.67, "temporal": true},
	]
	var rendering_device := RenderingServer.get_rendering_device()
	if rendering_device != null and rendering_device.has_feature(RenderingDevice.SUPPORTS_METALFX_SPATIAL):
		presentation_modes.push_back({"name": "metalfx_spatial", "mode": Viewport.SCALING_3D_MODE_METALFX_SPATIAL, "scale": 0.67, "temporal": false})
	if rendering_device != null and rendering_device.has_feature(RenderingDevice.SUPPORTS_METALFX_TEMPORAL):
		presentation_modes.push_back({"name": "metalfx_temporal", "mode": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL, "scale": 0.67, "temporal": true})
	if rendering_device != null and rendering_device.has_feature(RenderingDevice.SUPPORTS_METALFX_DENOISED):
		presentation_modes.push_back({
			"name": "metalfx_denoised",
			"mode": Viewport.SCALING_3D_MODE_BILINEAR,
			"scale": 0.67,
			"temporal": true,
			"denoiser": RenderingServer.PT_DENOISER_METALFX,
		})
	presentation_temporal_mode = presentation_modes[2]
	for mode: Dictionary in presentation_modes:
		if mode.name == "metalfx_temporal":
			presentation_temporal_mode = mode
		if mode.name == "metalfx_denoised":
			presentation_temporal_mode = mode


func _process_presentation() -> void:
	presentation_phase_frame += 1
	match presentation_phase:
		"matrix":
			if presentation_phase_frame == 1:
				_apply_presentation_mode(presentation_modes[presentation_mode_index], capture_viewport)
			if presentation_phase_frame >= PRESENTATION_MODE_WARMUP_FRAMES:
				_capture_presentation("presentation_%s" % presentation_modes[presentation_mode_index].name, capture_viewport)
				presentation_mode_index += 1
				presentation_phase_frame = 0
				if presentation_mode_index >= presentation_modes.size():
					presentation_phase = "before_cut"
					_apply_presentation_mode(presentation_temporal_mode, capture_viewport)
		"before_cut":
			if presentation_phase_frame >= PRESENTATION_TRANSITION_WARMUP_FRAMES:
				_capture_presentation("before_camera_cut", capture_viewport)
				capture_camera.fov = 62.0
				capture_camera.look_at_from_position(Vector3(-7.0, 5.0, -6.0), Vector3(0.0, 0.65, 0.0))
				_record_presentation_event("camera_cut")
				presentation_phase = "after_cut"
				presentation_phase_frame = 0
		"after_cut":
			if presentation_phase_frame >= PRESENTATION_TRANSITION_WARMUP_FRAMES:
				_capture_presentation("after_camera_cut", capture_viewport)
				_configure_capture_camera()
				DisplayServer.window_set_size(presentation_original_window_size + Vector2i(160, 96))
				_record_presentation_event("resize")
				presentation_phase = "after_resize"
				presentation_phase_frame = 0
		"after_resize":
			if presentation_phase_frame >= PRESENTATION_TRANSITION_WARMUP_FRAMES:
				_capture_presentation("after_resize", capture_viewport)
				get_tree().paused = true
				_record_presentation_event("editor_pause")
				presentation_phase = "paused"
				presentation_phase_frame = 0
		"paused":
			if presentation_phase_frame >= 20:
				get_tree().paused = false
				_record_presentation_event("editor_resume")
				presentation_phase = "after_resume"
				presentation_phase_frame = 0
		"after_resume":
			if presentation_phase_frame >= PRESENTATION_TRANSITION_WARMUP_FRAMES:
				_capture_presentation("after_pause_resume", capture_viewport)
				_create_presentation_secondary_viewport()
				_record_presentation_event("viewport_switch")
				presentation_phase = "secondary_viewport"
				presentation_phase_frame = 0
		"secondary_viewport":
			if presentation_phase_frame >= PRESENTATION_MODE_WARMUP_FRAMES:
				_capture_presentation("secondary_viewport", presentation_secondary_viewport)
				_record_presentation_event("viewport_return")
				presentation_phase = "viewport_return"
				presentation_phase_frame = 0
		"viewport_return":
			if presentation_phase_frame >= PRESENTATION_TRANSITION_WARMUP_FRAMES:
				_capture_presentation("after_viewport_return", capture_viewport)
				_finish_presentation()


func _apply_presentation_mode(mode: Dictionary, viewport: Viewport) -> void:
	environment.pathtracing_denoiser = mode.get("denoiser", RenderingServer.PT_DENOISER_NONE)
	viewport.scaling_3d_mode = mode.mode
	viewport.scaling_3d_scale = mode.scale
	_record_presentation_event("presentation_%s" % mode.name)


func _create_presentation_secondary_viewport() -> void:
	presentation_secondary_viewport = SubViewport.new()
	presentation_secondary_viewport.size = Vector2i(320, 320)
	presentation_secondary_viewport.world_3d = get_world_3d()
	presentation_secondary_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(presentation_secondary_viewport)
	var secondary_camera := Camera3D.new()
	presentation_secondary_viewport.add_child(secondary_camera)
	secondary_camera.fov = 48.0
	secondary_camera.look_at_from_position(Vector3(5.2, 3.8, 6.8), Vector3(0.0, 0.65, 0.0))
	secondary_camera.make_current()
	_apply_presentation_mode(presentation_temporal_mode, presentation_secondary_viewport)


func _capture_presentation(kind: String, viewport: Viewport) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := viewport.get_texture().get_image()
	if captured == null or captured.is_empty():
		_fail_presentation("PRESENTATION capture '%s' was empty." % kind)
		return
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var filename := "presentation_%s.png" % kind
	var path := artifact_dir.path_join(filename)
	var error := captured.save_png(path)
	if error != OK:
		_fail_presentation("Failed to save PRESENTATION capture %s: %s" % [path, error_string(error)])
		return
	presentation_captures[kind] = FileAccess.get_sha256(path)


func _record_presentation_event(event: String) -> void:
	presentation_events.push_back(event)
	print("METAL_RT_PRESENTATION_EVENT=%s" % event)


func _finish_presentation() -> void:
	get_tree().paused = false
	DisplayServer.window_set_size(presentation_original_window_size)
	var mode_names: Array[String] = []
	for mode: Dictionary in presentation_modes:
		mode_names.push_back(mode.name)
	var manifest := {
		"schema_version": 1,
		"fixture": "e0_hg0",
		"fixture_revision": FIXTURE_REVISION,
		"renderer": "forward_plus",
		"rendering_driver": "metal",
		"resolution": [CAPTURE_SIZE.x, CAPTURE_SIZE.y],
		"denoiser": "metalfx" if RenderingServer.is_pathtracing_denoiser_supported(RenderingServer.PT_DENOISER_METALFX) else "none",
		"native_denoising": "metalfx" if RenderingServer.is_pathtracing_denoiser_supported(RenderingServer.PT_DENOISER_METALFX) else "unavailable",
		"ser": "disabled",
		"presentation_modes": mode_names,
		"temporal_test_mode": presentation_temporal_mode.name,
		"events": presentation_events,
		"captures": presentation_captures,
		"sanitized_environment": "presentation_sanitized_environment.tres",
		"device": RenderingServer.get_video_adapter_name(),
		"os_version": OS.get_version(),
		"architecture": Engine.get_architecture_name(),
	}
	var manifest_path := artifact_dir.path_join("presentation_manifest.json")
	var file := FileAccess.open(manifest_path, FileAccess.WRITE)
	if file == null:
		_fail_presentation("Failed to create the presentation manifest.")
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
	print("METAL_RT_DENOISER_DEFAULT=none")
	print("METAL_RT_SER=disabled")
	print("METAL_RT_PRESENTATION=%s" % ",".join(mode_names))
	print("METAL_RT_TEMPORAL_SEQUENCE=passed")
	print("METAL_RT_PRESENTATION_MAC_UX=passed")
	get_tree().quit()


func _fail_presentation(message: String) -> bool:
	push_error("METAL_RT_PRESENTATION_MAC_UX=failed %s" % message)
	get_tree().quit(1)
	return false


func _capture(kind: String) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := capture_viewport.get_texture().get_image()
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var path := artifact_dir.path_join("e0_hg0_%s_%s.png" % [capture_label, kind])
	var error := captured.save_png(path)
	if error != OK:
		push_error("Failed to save EDITOR_SCENE editor capture %s: %s" % [path, error_string(error)])


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
		push_error("Failed to open EDITOR_SCENE manifest: %s" % manifest_path)
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
