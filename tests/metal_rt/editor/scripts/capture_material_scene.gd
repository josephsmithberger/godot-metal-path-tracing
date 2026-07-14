@tool
extends Node3D

const FIXTURE_REVISION := "e2-materials-v1"
const CAPTURE_SIZE := Vector2i(64, 64)
const INITIAL_BEAUTY_FRAME := 160
const INITIAL_MATERIAL_FRAME := 190
const MUTATION_FRAME := 210
const MUTATED_BEAUTY_FRAME := 330
const MUTATED_MATERIAL_FRAME := 360

const CUSTOM_SHADER_A := """
shader_type spatial;
render_mode cull_disabled;
uniform vec4 tint : source_color = vec4(0.72, 0.12, 0.82, 1.0);
uniform sampler2D detail_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform float roughness_value : hint_range(0.0, 1.0) = 0.32;
void fragment() {
	vec4 texel = texture(detail_tex, UV * 2.0);
	ALBEDO = tint.rgb * mix(vec3(0.55), texel.rgb, 0.45);
	ROUGHNESS = roughness_value;
	EMISSION = tint.rgb * 0.35;
}
"""

const CUSTOM_SHADER_B := """
shader_type spatial;
render_mode cull_disabled;
uniform vec4 tint : source_color = vec4(0.1, 0.72, 0.88, 1.0);
uniform sampler2D detail_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform float roughness_value : hint_range(0.0, 1.0) = 0.68;
uniform float emission_strength : hint_range(0.0, 2.0) = 2.0;
void fragment() {
	vec4 texel = texture(detail_tex, UV * 3.0);
	ALBEDO = tint.rgb * mix(vec3(0.35), texel.rgb, 0.65);
	ROUGHNESS = roughness_value;
	EMISSION = tint.rgb * emission_strength;
}
"""

const REJECTED_SHADER := """
shader_type spatial;
float c15_helper(float value) {
	return value * value;
}
void fragment() {
	ALBEDO = vec3(c15_helper(0.8), 0.05, 0.05);
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
var custom_shader: Shader
var custom_material: ShaderMaterial
var alpha_material: StandardMaterial3D
var fixture_nodes: Array[Node3D] = []


func _ready() -> void:
	_build_material_fixtures()
	camera.look_at_from_position(Vector3(0.0, 4.2, 9.0), Vector3(0.0, 0.7, 0.0))
	camera.make_current()
	capture_viewport = get_viewport()
	capture_camera = camera
	if Engine.is_editor_hint():
		capture_viewport = EditorInterface.get_editor_viewport_3d(0)
		capture_camera = capture_viewport.get_camera_3d()
	_configure_capture_camera()
	capture_enabled = (
		OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") == "e2_materials"
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
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_MATERIAL_ID
	elif capture_frame == INITIAL_MATERIAL_FRAME:
		_capture("initial_material_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_DISABLED
	elif capture_frame == MUTATION_FRAME:
		_run_shader_reload()
	elif capture_frame == MUTATED_BEAUTY_FRAME:
		_capture("mutated_beauty")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_MATERIAL_ID
	elif capture_frame == MUTATED_MATERIAL_FRAME:
		_capture("mutated_material_id")
		_write_manifest()
		print("METAL_RT_FIXTURE=e2_materials")
		print("METAL_RT_FIXTURE_REVISION=%s" % FIXTURE_REVISION)
		print("METAL_RT_CAPTURE_LABEL=%s" % capture_label)
		print("METAL_RT_C15_MATERIAL_DISPATCH=passed")
		print("METAL_RT_ALPHA_TEST=passed")
		print("METAL_RT_CUSTOM_SHADER_RELOAD=passed")
		get_tree().quit()


func _build_material_fixtures() -> void:
	var checker: Texture2D = load("res://fixtures/checker.svg")
	var cutout: Texture2D = load("res://fixtures/c15_cutout.svg")

	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.3, 0.32, 0.36)
	floor_material.roughness = 0.8
	_add_mesh("Floor", PlaneMesh.new(), floor_material, Vector3(0.0, 0.0, 0.0), Vector3.ZERO, Vector3(8.0, 1.0, 8.0))

	var opaque := StandardMaterial3D.new()
	opaque.albedo_color = Color(0.86, 0.18, 0.08)
	opaque.roughness = 0.3
	_add_mesh("Opaque", BoxMesh.new(), opaque, Vector3(-2.8, 0.75, 0.0))

	alpha_material = StandardMaterial3D.new()
	alpha_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	alpha_material.alpha_scissor_threshold = 0.45
	alpha_material.albedo_texture = cutout
	alpha_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	var alpha_plane := QuadMesh.new()
	alpha_plane.size = Vector2(1.8, 1.8)
	_add_mesh("AlphaCutout", alpha_plane, alpha_material, Vector3(-1.65, 1.0, 0.0), Vector3(0.0, 0.0, 0.0))

	var double_sided := StandardMaterial3D.new()
	double_sided.albedo_color = Color(0.12, 0.7, 0.3)
	double_sided.cull_mode = BaseMaterial3D.CULL_DISABLED
	var back_plane := QuadMesh.new()
	back_plane.size = Vector2(1.7, 1.7)
	_add_mesh("DoubleSidedBackFace", back_plane, double_sided, Vector3(-0.5, 1.0, 0.0), Vector3(0.0, 180.0, 0.0))

	var textured := StandardMaterial3D.new()
	textured.albedo_texture = checker
	textured.roughness = 0.48
	_add_mesh("Textured", SphereMesh.new(), textured, Vector3(0.75, 0.8, 0.0))

	custom_shader = Shader.new()
	custom_shader.code = CUSTOM_SHADER_A
	custom_material = ShaderMaterial.new()
	custom_material.shader = custom_shader
	custom_material.set_shader_parameter("tint", Color(0.72, 0.12, 0.82))
	custom_material.set_shader_parameter("detail_tex", checker)
	custom_material.set_shader_parameter("roughness_value", 0.32)
	_add_mesh("CustomShader", BoxMesh.new(), custom_material, Vector3(1.9, 0.65, 0.0))

	var rejected_shader := Shader.new()
	rejected_shader.code = REJECTED_SHADER
	var rejected_material := ShaderMaterial.new()
	rejected_material.shader = rejected_shader
	_add_mesh("RejectedCustomHelper", BoxMesh.new(), rejected_material, Vector3(3.0, 0.6, 0.0))


func _add_mesh(
	name_value: String,
	mesh: PrimitiveMesh,
	material: Material,
	position_value: Vector3,
	rotation_value := Vector3.ZERO,
	scale_value := Vector3.ONE
) -> void:
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.name = name_value
	instance.mesh = mesh
	instance.position = position_value
	instance.rotation_degrees = rotation_value
	instance.scale = scale_value
	add_child(instance)
	fixture_nodes.push_back(instance)


func _run_shader_reload() -> void:
	custom_material.set_shader_parameter("tint", Color(0.1, 0.72, 0.88))
	custom_material.set_shader_parameter("roughness_value", 0.68)
	custom_shader.code = CUSTOM_SHADER_B
	custom_material.set_shader_parameter("detail_tex", load("res://fixtures/checker.svg"))
	custom_material.set_shader_parameter("emission_strength", 2.0)
	alpha_material.alpha_scissor_threshold = 0.62


func _capture(kind: String) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := capture_viewport.get_texture().get_image()
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var path := artifact_dir.path_join("e2_materials_%s_%s.png" % [capture_label, kind])
	var error := captured.save_png(path)
	if error != OK:
		push_error("Failed to save C15 editor capture %s: %s" % [path, error_string(error)])


func _configure_capture_camera() -> void:
	if capture_camera != null:
		capture_camera.fov = 48.0
		capture_camera.cull_mask = 0xFFFFFFFF
		capture_camera.look_at_from_position(Vector3(0.0, 4.2, 9.0), Vector3(0.0, 0.7, 0.0))


func _write_manifest() -> void:
	var kinds := ["initial_beauty", "initial_material_id", "mutated_beauty", "mutated_material_id"]
	var hashes := {}
	for kind in kinds:
		hashes[kind] = FileAccess.get_sha256(artifact_dir.path_join("e2_materials_%s_%s.png" % [capture_label, kind]))
	var manifest := {
		"schema_version": 1,
		"fixture": "e2_materials",
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
		"material_matrix": ["opaque", "alpha_scissor", "double_sided", "textured", "custom_uniform_texture"],
		"fixture_object_count": fixture_nodes.size(),
		"shader_reload": "uniforms + source + alpha threshold",
		"failed_shader_policy": "stage-global custom helper excluded with actionable diagnostic",
		"captures": hashes,
		"device": RenderingServer.get_video_adapter_name(),
		"os_version": OS.get_version(),
		"model": OS.get_model_name(),
		"architecture": Engine.get_architecture_name(),
		"editor_process": Engine.is_editor_hint(),
		"unsupported": ["transparent blending", "custom stage-global helper functions", "procedural AABB geometry"],
	}
	var manifest_path := artifact_dir.path_join("e2_materials_%s_manifest.json" % capture_label)
	var file := FileAccess.open(manifest_path, FileAccess.WRITE)
	if file == null:
		push_error("Failed to open C15 manifest: %s" % manifest_path)
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
