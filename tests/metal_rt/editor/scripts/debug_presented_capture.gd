@tool
extends Node3D

# Debug aid for the Metal RT sync investigation: captures the editor 3D
# viewport at FULL resolution (the shipping fixtures downscale to 64x64,
# which averages away 8x4-pixel SIMD-group tearing).
#
# Modes via GODOT_DBG_CAPTURE_MODE:
#   camera (default) - orbit the camera each frame, then hold still.
#   light            - camera fixed, KeyLight rotates each frame (sky/sun
#                      content changes while camera-derived data is static).
#
# Enabled only when GODOT_DBG_CAPTURE_DIR is set. Saves moving_<f>.png during
# the animated phase and static_<f>.png after it stops, then quits.

const MOVE_END_FRAME := 240
const QUIT_FRAME := 420
const MOVING_CAPTURE_FRAMES := [90, 150, 209, 210, 211, 240]
const STATIC_CAPTURE_FRAMES := [300, 360, 415]

var frame := 0
var capture_dir := ""
var mode := "camera"

func _ready() -> void:
	capture_dir = OS.get_environment("GODOT_DBG_CAPTURE_DIR")
	mode = OS.get_environment("GODOT_DBG_CAPTURE_MODE")
	if mode.is_empty():
		mode = "camera"
	var spp := OS.get_environment("GODOT_DBG_SPP")
	var bounces := OS.get_environment("GODOT_DBG_BOUNCES")
	var vis := OS.get_environment("GODOT_DBG_VIS")
	if not spp.is_empty() or not bounces.is_empty() or not vis.is_empty():
		var world_env: WorldEnvironment = find_children("*", "WorldEnvironment", true, false)[0]
		if not spp.is_empty():
			world_env.environment.pathtracing_samples_per_pixel = int(spp)
		if not bounces.is_empty():
			world_env.environment.pathtracing_max_bounces = int(bounces)
		if not vis.is_empty():
			world_env.environment.pathtracing_debug_mode = int(vis)
	set_process(Engine.is_editor_hint() and not capture_dir.is_empty())

func _process(_delta: float) -> void:
	frame += 1
	var viewport := EditorInterface.get_editor_viewport_3d(0)
	var camera := viewport.get_camera_3d()
	if camera == null:
		return
	if mode == "light":
		camera.look_at_from_position(Vector3(0.0, 4.1, 9.0), Vector3(0.0, 0.75, 0.0))
		if frame <= MOVE_END_FRAME:
			var lights := find_children("*", "DirectionalLight3D", true, false)
			var light: DirectionalLight3D = lights[0] if not lights.is_empty() else null
			if light != null:
				var t := float(frame) * 0.01
				light.rotation_degrees = Vector3(-52.0 + sin(t) * 20.0, -30.0 + t * 57.29578 * 0.2, 0.0)
	else:
		if frame <= MOVE_END_FRAME:
			var t := float(frame) * 0.02
			var pos := Vector3(sin(t) * 1.5, 4.1 + sin(t * 1.7) * 0.4, 9.0 + cos(t) * 0.8)
			camera.look_at_from_position(pos, Vector3(0.0, 0.75, 0.0))
	if frame in MOVING_CAPTURE_FRAMES:
		_save("moving_%d" % frame, viewport)
	elif frame in STATIC_CAPTURE_FRAMES:
		_save("static_%d" % frame, viewport)
	if frame >= QUIT_FRAME:
		get_tree().quit()

func _save(label: String, viewport: Viewport) -> void:
	DirAccess.make_dir_recursive_absolute(capture_dir)
	var image := viewport.get_texture().get_image()
	var path := capture_dir.path_join("%s.png" % label)
	var err := image.save_png(path)
	print("DBG_CAPTURE saved=%s size=%dx%d err=%d" % [path, image.get_width(), image.get_height(), err])
