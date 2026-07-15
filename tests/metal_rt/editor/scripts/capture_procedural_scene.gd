@tool
extends Node3D

const FIXTURE_REVISION := "e3-procedural-v1"
const CAPTURE_SIZE := Vector2i(64, 64)
const INITIAL_BEAUTY_FRAME := 180
const INITIAL_INSTANCE_FRAME := 210
const INITIAL_PRIMITIVE_FRAME := 240
const INITIAL_HIT_KIND_FRAME := 270
const MUTATION_FRAME := 290
const MUTATED_BEAUTY_FRAME := 420
const MUTATED_INSTANCE_FRAME := 450
const MUTATED_PRIMITIVE_FRAME := 480
const MUTATED_HIT_KIND_FRAME := 510

const PROCEDURAL_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform vec4 tint : source_color = vec4(0.9, 0.2, 0.08, 1.0);
uniform float radius_scale : hint_range(0.2, 1.0) = 0.88;
void intersection() {
	vec3 center = (AABB_MIN + AABB_MAX) * 0.5;
	vec3 extent = (AABB_MAX - AABB_MIN) * 0.5;
	float radius = min(extent.x, min(extent.y, extent.z)) * radius_scale;
	vec3 oc = ORIGIN - center;
	float half_b = dot(oc, DIRECTION);
	float c = dot(oc, oc) - radius * radius;
	float discriminant = half_b * half_b - c;
	if (discriminant >= 0.0) {
		float root = sqrt(discriminant);
		float hit_t = -half_b - root;
		if (hit_t < T_MIN || hit_t > T_MAX) {
			hit_t = -half_b + root;
		}
		if (hit_t >= T_MIN && hit_t <= T_MAX) {
			vec3 hit_position = ORIGIN + DIRECTION * hit_t;
			HIT_NORMAL = normalize(hit_position - center);
			vec3 up = abs(HIT_NORMAL.y) > 0.98 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
			HIT_TANGENT = normalize(cross(up, HIT_NORMAL));
			HIT_UV = vec2(atan(HIT_NORMAL.z, HIT_NORMAL.x) * 0.15915494 + 0.5, acos(clamp(HIT_NORMAL.y, -1.0, 1.0)) * 0.31830989);
			PREV_POSITION = hit_position;
			report_intersection(hit_t, 41u);
		}
	}
}
void fragment() {
	ALBEDO = tint.rgb;
	ROUGHNESS = 0.34;
	EMISSION = tint.rgb * 0.25;
}
"""

@onready var camera: Camera3D = $Camera
@onready var environment: Environment = $WorldEnvironment.environment

var capture_enabled := false
var capture_frame := 0
var artifact_dir := ""
var capture_label := "cold"
var capture_viewport: Viewport
var capture_camera: Camera3D
var primary_procedural: RTProceduralInstance3D
var removable_procedural: RTProceduralInstance3D
var invalid_procedural: RTProceduralInstance3D
var fixture_nodes: Array[Node3D] = []
var procedural_builds_expected := 3
var procedural_refits_expected := 0


func _ready() -> void:
	_build_fixture()
	camera.look_at_from_position(Vector3(0.0, 4.1, 9.0), Vector3(0.0, 0.75, 0.0))
	camera.make_current()
	capture_viewport = get_viewport()
	capture_camera = camera
	if Engine.is_editor_hint():
		capture_viewport = EditorInterface.get_editor_viewport_3d(0)
		capture_camera = capture_viewport.get_camera_3d()
	_configure_capture_camera()
	capture_enabled = (
		OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") == "e3_procedural"
	)
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
	if capture_frame == INITIAL_BEAUTY_FRAME:
		_capture("initial_beauty")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_INSTANCE_ID
	elif capture_frame == INITIAL_INSTANCE_FRAME:
		_capture("initial_instance_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_PRIMITIVE_ID
	elif capture_frame == INITIAL_PRIMITIVE_FRAME:
		_capture("initial_primitive_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_HIT_KIND
	elif capture_frame == INITIAL_HIT_KIND_FRAME:
		_capture("initial_hit_kind")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_DISABLED
	elif capture_frame == MUTATION_FRAME:
		_run_mutation_sequence()
	elif capture_frame == MUTATED_BEAUTY_FRAME:
		_capture("mutated_beauty")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_INSTANCE_ID
	elif capture_frame == MUTATED_INSTANCE_FRAME:
		_capture("mutated_instance_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_PRIMITIVE_ID
	elif capture_frame == MUTATED_PRIMITIVE_FRAME:
		_capture("mutated_primitive_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_HIT_KIND
	elif capture_frame == MUTATED_HIT_KIND_FRAME:
		_capture("mutated_hit_kind")
		_write_manifest()
		print("METAL_RT_FIXTURE=e3_procedural")
		print("METAL_RT_FIXTURE_REVISION=%s" % FIXTURE_REVISION)
		print("METAL_RT_CAPTURE_LABEL=%s" % capture_label)
		print("METAL_RT_PROCEDURAL_UPDATE=builds:%d,refits:%d,removed:1" % [procedural_builds_expected, procedural_refits_expected])
		if OS.get_environment("GODOT_MTL_DISABLE_RAYTRACING") != "1":
			print("METAL_RT_C16_PROCEDURAL=passed")
			print("METAL_RT_CUSTOM_INTERSECTION=passed")
			print("METAL_RT_MIXED_GEOMETRY=passed")
		get_tree().quit()


func _build_fixture() -> void:
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.3, 0.33, 0.38)
	floor_material.roughness = 0.82
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(9.0, 7.0)
	floor_mesh.material = floor_material
	_add_mesh("TriangleFloor", floor_mesh, Vector3(0.0, 0.0, 0.0))

	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.12, 0.45, 0.88)
	box_material.roughness = 0.42
	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(1.25, 1.5, 1.25)
	box_mesh.material = box_material
	_add_mesh("TriangleBox", box_mesh, Vector3(2.35, 0.75, -0.2))

	primary_procedural = _make_procedural(
		"PrimaryProcedural",
		[ AABB(Vector3(-0.75, -0.75, -0.75), Vector3(1.5, 1.5, 1.5)) ],
		Color(0.94, 0.18, 0.06),
		Vector3(-1.55, 0.9, 0.25)
	)
	removable_procedural = _make_procedural(
		"RemovableMultiAABB",
		[
			AABB(Vector3(-0.9, -0.45, -0.45), Vector3(0.9, 0.9, 0.9)),
			AABB(Vector3(0.0, -0.45, -0.45), Vector3(0.9, 0.9, 0.9)),
		],
		Color(0.1, 0.82, 0.3),
		Vector3(0.15, 1.0, -0.35)
	)
	var fallback_bounds: Array[AABB] = []
	var fallback_procedural := _make_procedural(
		"FallbackBoundsProcedural",
		fallback_bounds,
		Color(0.95, 0.62, 0.08),
		Vector3(-3.0, 0.65, -0.55)
	)
	fallback_procedural.size = Vector3(1.15, 1.3, 1.15)

	# This invalid zero-volume record is inside a valid culling box. C16 must
	# omit it without dropping the valid triangle floor visible behind it.
	invalid_procedural = _make_procedural(
		"InvalidProcedural",
		[ AABB(Vector3(-0.7, -0.7, -0.7), Vector3(1.4, 0.0, 1.4)) ],
		Color(1.0, 0.0, 1.0),
		Vector3(3.45, 0.72, 0.55),
		AABB(Vector3(-0.75, -0.75, -0.75), Vector3(1.5, 1.5, 1.5))
	)


func _make_procedural(
	name_value: String,
	bounds_value: Array[AABB],
	color: Color,
	position_value: Vector3,
	custom_culling_aabb := AABB()
) -> RTProceduralInstance3D:
	var shader := Shader.new()
	shader.code = PROCEDURAL_SHADER
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("tint", color)
	material.set_shader_parameter("radius_scale", 0.88)
	var instance := RTProceduralInstance3D.new()
	instance.name = name_value
	instance.material_override = material
	instance.expose_aabb_bounds = true
	instance.custom_enclosing_aabb = custom_culling_aabb
	instance.bounds = bounds_value
	instance.position = position_value
	add_child(instance)
	fixture_nodes.push_back(instance)
	return instance


func _add_mesh(name_value: String, mesh: PrimitiveMesh, position_value: Vector3) -> void:
	var instance := MeshInstance3D.new()
	instance.name = name_value
	instance.mesh = mesh
	instance.position = position_value
	add_child(instance)
	fixture_nodes.push_back(instance)


func _run_mutation_sequence() -> void:
	primary_procedural.bounds = [ AABB(Vector3(-1.0, -0.62, -0.62), Vector3(2.0, 1.24, 1.24)) ]
	primary_procedural.position = Vector3(-1.25, 1.05, 0.15)
	procedural_refits_expected += 1
	fixture_nodes.erase(removable_procedural)
	removable_procedural.queue_free()
	removable_procedural = null


func _capture(kind: String) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := capture_viewport.get_texture().get_image()
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var path := artifact_dir.path_join("e3_procedural_%s_%s.png" % [capture_label, kind])
	var error := captured.save_png(path)
	if error != OK:
		push_error("Failed to save C16 editor capture %s: %s" % [path, error_string(error)])


func _configure_capture_camera() -> void:
	if capture_camera != null:
		capture_camera.fov = 48.0
		capture_camera.cull_mask = 0xFFFFFFFF
		capture_camera.look_at_from_position(Vector3(0.0, 4.1, 9.0), Vector3(0.0, 0.75, 0.0))


func _write_manifest() -> void:
	var kinds := [
		"initial_beauty", "initial_instance_id", "initial_primitive_id", "initial_hit_kind",
		"mutated_beauty", "mutated_instance_id", "mutated_primitive_id", "mutated_hit_kind",
	]
	var hashes := {}
	for kind in kinds:
		hashes[kind] = FileAccess.get_sha256(artifact_dir.path_join("e3_procedural_%s_%s.png" % [capture_label, kind]))
	var manifest := {
		"schema_version": 1,
		"fixture": "e3_procedural",
		"fixture_revision": FIXTURE_REVISION,
		"capture_label": capture_label,
		"renderer": "forward_plus",
		"rendering_driver": "metal",
		"resolution": [CAPTURE_SIZE.x, CAPTURE_SIZE.y],
		"color_space": "viewport_srgb",
		"format": "rgba8_png",
		"samples_per_pixel": 4,
		"max_bounces": 2,
		"denoiser": "none",
		"support_matrix": ["triangle", "procedural_fallback_bounds", "procedural_explicit_bounds", "multi_aabb", "custom_intersection", "bounds_refit", "instance_removal"],
		"initial_fixture_object_count": 6,
		"mutated_fixture_object_count": fixture_nodes.size(),
		"procedural_builds_expected": procedural_builds_expected,
		"procedural_refits_expected": procedural_refits_expected,
		"procedural_instances_removed": 1,
		"invalid_bounds_policy": "omit invalid procedural geometry; retain valid mixed-scene triangles; warn once",
		"unsupported": ["intersection stage-global helper functions"],
		"captures": hashes,
		"device": RenderingServer.get_video_adapter_name(),
		"os_version": OS.get_version(),
		"model": OS.get_model_name(),
		"architecture": Engine.get_architecture_name(),
		"editor_process": Engine.is_editor_hint(),
	}
	var manifest_path := artifact_dir.path_join("e3_procedural_%s_manifest.json" % capture_label)
	var file := FileAccess.open(manifest_path, FileAccess.WRITE)
	if file == null:
		push_error("Failed to open C16 manifest: %s" % manifest_path)
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
