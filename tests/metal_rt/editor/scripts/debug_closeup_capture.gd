@tool
extends Node3D

# One-shot close-up capture for artifact investigation.
# Env vars:
#   GODOT_DBG_CAPTURE_DIR  - output dir (required to enable)
#   GODOT_DBG_CAM_POS      - "x,y,z" camera position
#   GODOT_DBG_CAM_LOOK     - "x,y,z" look-at target
#   GODOT_DBG_CAM_FOV      - fov degrees (default 40)
#   GODOT_DBG_SPP / GODOT_DBG_BOUNCES / GODOT_DBG_VIS - pathtracing overrides

const CAPTURE_FRAME := 240
const QUIT_FRAME := 250

var frame := 0
var capture_dir := ""
var cam_pos := Vector3(2.2, 1.0, 1.4)
var cam_look := Vector3(1.15, 0.85, -0.45)
var cam_fov := 40.0

static func _parse_vec3(s: String, fallback: Vector3) -> Vector3:
	var parts := s.split(",")
	if parts.size() != 3:
		return fallback
	return Vector3(parts[0].to_float(), parts[1].to_float(), parts[2].to_float())

func _ready() -> void:
	capture_dir = OS.get_environment("GODOT_DBG_CAPTURE_DIR")
	cam_pos = _parse_vec3(OS.get_environment("GODOT_DBG_CAM_POS"), cam_pos)
	cam_look = _parse_vec3(OS.get_environment("GODOT_DBG_CAM_LOOK"), cam_look)
	var fov_env := OS.get_environment("GODOT_DBG_CAM_FOV")
	if not fov_env.is_empty():
		cam_fov = fov_env.to_float()
	if OS.get_environment("GODOT_DBG_DISABLE_PT") == "1":
		var wenv: WorldEnvironment = find_children("*", "WorldEnvironment", true, false)[0]
		wenv.environment.pathtracing_enabled = false
	if OS.get_environment("GODOT_DBG_MIRROR_SPHERE") == "1":
		var sphere: MeshInstance3D = find_children("Sphere", "MeshInstance3D", true, false)[0]
		var chrome := StandardMaterial3D.new()
		chrome.albedo_color = Color(1, 1, 1)
		chrome.metallic = 1.0
		chrome.roughness = 0.0
		sphere.material_override = chrome
	var mat_env := OS.get_environment("GODOT_DBG_SPHERE_MAT")
	if not mat_env.is_empty():
		# "metallic,roughness[,albedo_r,albedo_g,albedo_b]"
		var p := mat_env.split(",")
		var sphere2: MeshInstance3D = find_children("Sphere", "MeshInstance3D", true, false)[0]
		var m := StandardMaterial3D.new()
		m.albedo_color = Color(0.08, 0.42, 0.72) if p.size() < 5 else Color(p[2].to_float(), p[3].to_float(), p[4].to_float())
		m.metallic = p[0].to_float()
		m.roughness = p[1].to_float()
		sphere2.material_override = m
	var seg_env := OS.get_environment("GODOT_DBG_SPHERE_SEGMENTS")
	if not seg_env.is_empty():
		# "radial,rings"
		var s := seg_env.split(",")
		var sphere3: MeshInstance3D = find_children("Sphere", "MeshInstance3D", true, false)[0]
		var sm: SphereMesh = sphere3.mesh
		sm.radial_segments = int(s[0])
		sm.rings = int(s[1])
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
	camera.fov = cam_fov
	camera.look_at_from_position(cam_pos, cam_look)
	if frame == CAPTURE_FRAME:
		DirAccess.make_dir_recursive_absolute(capture_dir)
		var image := viewport.get_texture().get_image()
		var path := capture_dir.path_join("closeup.png")
		var err := image.save_png(path)
		print("DBG_CAPTURE saved=%s size=%dx%d err=%d" % [path, image.get_width(), image.get_height(), err])
	if frame >= QUIT_FRAME:
		get_tree().quit()
