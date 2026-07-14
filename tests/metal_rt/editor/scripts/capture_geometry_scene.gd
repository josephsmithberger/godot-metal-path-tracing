@tool
extends Node3D

const FIXTURE_REVISION := "e1-geometry-v1"
const CAPTURE_SIZE := Vector2i(64, 64)
const INITIAL_BEAUTY_FRAME := 120
const INITIAL_INSTANCE_FRAME := 145
const INITIAL_PRIMITIVE_FRAME := 170
const MUTATION_FRAME := 190
const RESTORE_VISIBILITY_FRAME := 205
const MUTATED_BEAUTY_FRAME := 270
const MUTATED_INSTANCE_FRAME := 295
const MUTATED_PRIMITIVE_FRAME := 320

@onready var camera: Camera3D = $Camera
@onready var environment: Environment = $WorldEnvironment.environment

var capture_enabled := false
var capture_frame := 0
var artifact_dir := ""
var capture_label := "cold"
var capture_viewport: Viewport
var capture_camera: Camera3D
var fixture_nodes: Array[Node3D] = []
var deformed_mesh_instance: MeshInstance3D
var multimesh_instance: MultiMeshInstance3D
var visibility_fixture: MeshInstance3D


func _ready() -> void:
	if (
		Engine.is_editor_hint()
		and OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") in ["e0_hg0", "e2_materials"]
	):
		var fixture := OS.get_environment("GODOT_MRT_FIXTURE")
		EditorInterface.call_deferred("open_scene_from_path", "res://fixtures/%s.tscn" % fixture)
		return
	_build_geometry_fixtures()
	camera.look_at_from_position(Vector3(7.6, 5.2, 9.4), Vector3(0.0, 0.6, 0.0))
	camera.make_current()
	capture_viewport = get_viewport()
	capture_camera = camera
	if Engine.is_editor_hint():
		capture_viewport = EditorInterface.get_editor_viewport_3d(0)
		capture_camera = capture_viewport.get_camera_3d()
	_configure_capture_camera()
	capture_enabled = (
		OS.get_environment("GODOT_MRT_EDITOR_CAPTURE") == "1"
		and OS.get_environment("GODOT_MRT_FIXTURE") == "e1_geometry"
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
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_DISABLED
	elif capture_frame == MUTATION_FRAME:
		_run_mutation_sequence()
	elif capture_frame == RESTORE_VISIBILITY_FRAME:
		visibility_fixture.visible = true
	elif capture_frame == MUTATED_BEAUTY_FRAME:
		_capture("mutated_beauty")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_INSTANCE_ID
	elif capture_frame == MUTATED_INSTANCE_FRAME:
		_capture("mutated_instance_id")
		environment.pathtracing_debug_mode = Environment.RT_DEBUG_PRIMITIVE_ID
	elif capture_frame == MUTATED_PRIMITIVE_FRAME:
		_capture("mutated_primitive_id")
		_write_manifest()
		print("METAL_RT_FIXTURE=e1_geometry")
		print("METAL_RT_FIXTURE_REVISION=%s" % FIXTURE_REVISION)
		print("METAL_RT_CAPTURE_LABEL=%s" % capture_label)
		print("METAL_RT_MUTATION_SEQUENCE=passed")
		if OS.get_environment("GODOT_MTL_DISABLE_RAYTRACING") != "1":
			print("METAL_RT_C14_SCENE_GEOMETRY=passed")
		get_tree().quit()


func _build_geometry_fixtures() -> void:
	var indexed := MeshInstance3D.new()
	indexed.name = "Indexed"
	indexed.mesh = _make_indexed_mesh(_material(Color(0.85, 0.18, 0.08)))
	indexed.position = Vector3(-2.6, 0.8, 0.2)
	_add_fixture(indexed)

	var nonindexed := MeshInstance3D.new()
	nonindexed.name = "NonIndexed"
	nonindexed.mesh = _make_nonindexed_mesh(_material(Color(0.12, 0.65, 0.24)), false)
	nonindexed.position = Vector3(-0.9, 0.9, 0.0)
	_add_fixture(nonindexed)

	var compressed := MeshInstance3D.new()
	compressed.name = "Compressed"
	compressed.mesh = _make_nonindexed_mesh(_material(Color(0.14, 0.38, 0.9)), true)
	compressed.position = Vector3(0.9, 0.9, -0.1)
	compressed.scale = Vector3(1.25, 1.25, 1.25)
	_add_fixture(compressed)

	deformed_mesh_instance = MeshInstance3D.new()
	deformed_mesh_instance.name = "BlendShapeDeformed"
	deformed_mesh_instance.mesh = _make_blend_shape_mesh(_material(Color(0.9, 0.58, 0.08)))
	deformed_mesh_instance.position = Vector3(2.6, 0.85, 0.0)
	deformed_mesh_instance.set_blend_shape_value(0, 0.2)
	_add_fixture(deformed_mesh_instance)

	multimesh_instance = MultiMeshInstance3D.new()
	multimesh_instance.name = "RepeatedMultiMesh"
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.instance_count = 3
	var repeated_mesh := BoxMesh.new()
	repeated_mesh.size = Vector3(0.65, 0.65, 0.65)
	repeated_mesh.material = _material(Color(0.62, 0.18, 0.78))
	multimesh.mesh = repeated_mesh
	multimesh.set_instance_transform(0, Transform3D(Basis.IDENTITY, Vector3(-1.1, 0.35, -2.0)))
	multimesh.set_instance_transform(1, Transform3D(Basis.from_scale(Vector3(-1.0, 1.0, 1.0)), Vector3(0.0, 0.35, -2.0)))
	multimesh.set_instance_transform(2, Transform3D(Basis.IDENTITY, Vector3(1.1, 0.35, -2.0)))
	multimesh_instance.multimesh = multimesh
	_add_fixture(multimesh_instance)

	visibility_fixture = MeshInstance3D.new()
	visibility_fixture.name = "VisibilityAndNegativeScale"
	var visibility_mesh := BoxMesh.new()
	visibility_mesh.size = Vector3(0.85, 1.3, 0.85)
	visibility_mesh.material = _material(Color(0.08, 0.72, 0.72))
	visibility_fixture.mesh = visibility_mesh
	visibility_fixture.position = Vector3(0.0, 0.65, 1.8)
	visibility_fixture.scale = Vector3(-1.0, 1.0, 1.0)
	visibility_fixture.layers = 2
	_add_fixture(visibility_fixture)


func _add_fixture(node: Node3D) -> void:
	add_child(node)
	fixture_nodes.push_back(node)


func _make_indexed_mesh(material: Material) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.75, -0.75, 0.0), Vector3(0.75, -0.75, 0.0),
		Vector3(0.75, 0.75, 0.0), Vector3(-0.75, 0.75, 0.0),
	])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 2, 1, 0, 3, 2])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, material)
	return mesh


func _make_nonindexed_mesh(material: Material, compressed: bool) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.8, -0.7, 0.0), Vector3(0.0, 0.85, 0.0), Vector3(0.8, -0.7, 0.0),
	])
	var flags := Mesh.ARRAY_FLAG_COMPRESS_ATTRIBUTES if compressed else 0
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, flags)
	mesh.surface_set_material(0, material)
	return mesh


func _make_blend_shape_mesh(material: Material) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	mesh.add_blend_shape("Raise")
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.75, -0.7, 0.0), Vector3(0.0, 0.8, 0.0), Vector3(0.75, -0.7, 0.0),
	])
	var shape := []
	shape.resize(Mesh.ARRAY_MAX)
	shape[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3.ZERO, Vector3(0.0, 0.75, 0.35), Vector3.ZERO,
	])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [shape])
	mesh.surface_set_material(0, material)
	return mesh


func _material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.42
	return material


func _run_mutation_sequence() -> void:
	# Remove and re-add every fixture node without restarting the editor. The
	# reverse order changes scene-instance ordering and catches stale user IDs.
	var transforms: Array[Transform3D] = []
	for node in fixture_nodes:
		transforms.push_back(node.transform)
		remove_child(node)
	for index in range(fixture_nodes.size() - 1, -1, -1):
		var node := fixture_nodes[index]
		add_child(node)
		node.transform = transforms[index]

	fixture_nodes[0].position.y += 0.2
	fixture_nodes[1].rotation_degrees.y = 18.0
	deformed_mesh_instance.set_blend_shape_value(0, 0.85)
	var mm := multimesh_instance.multimesh
	mm.set_instance_transform(2, Transform3D(Basis.IDENTITY, Vector3(1.35, 0.65, -1.8)))
	visibility_fixture.visible = false


func _capture(kind: String) -> void:
	DirAccess.make_dir_recursive_absolute(artifact_dir)
	var captured := capture_viewport.get_texture().get_image()
	captured.convert(Image.FORMAT_RGBA8)
	captured.resize(CAPTURE_SIZE.x, CAPTURE_SIZE.y, Image.INTERPOLATE_LANCZOS)
	var path := artifact_dir.path_join("e1_geometry_%s_%s.png" % [capture_label, kind])
	var error := captured.save_png(path)
	if error != OK:
		push_error("Failed to save C14 editor capture %s: %s" % [path, error_string(error)])


func _configure_capture_camera() -> void:
	if capture_camera != null:
		capture_camera.fov = 48.0
		capture_camera.cull_mask = 0xFFFFFFFF
		capture_camera.look_at_from_position(Vector3(7.6, 5.2, 9.4), Vector3(0.0, 0.6, 0.0))


func _write_manifest() -> void:
	var kinds := [
		"initial_beauty", "initial_instance_id", "initial_primitive_id",
		"mutated_beauty", "mutated_instance_id", "mutated_primitive_id",
	]
	var hashes := {}
	for kind in kinds:
		hashes[kind] = FileAccess.get_sha256(artifact_dir.path_join("e1_geometry_%s_%s.png" % [capture_label, kind]))
	var manifest := {
		"schema_version": 1,
		"fixture": "e1_geometry",
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
		"geometry_matrix": [
			"indexed_float3", "nonindexed_float3", "compressed_unorm16x4",
			"blend_shape_deformed_refit", "multimesh_repeated", "negative_scale",
		],
		"fixture_object_count": fixture_nodes.size(),
		"multimesh_instance_count": multimesh_instance.multimesh.instance_count,
		"mutation_sequence": "remove/re-add all; transforms; visibility; blend shape; MultiMesh transform",
		"captures": hashes,
		"device": RenderingServer.get_video_adapter_name(),
		"os_version": OS.get_version(),
		"model": OS.get_model_name(),
		"architecture": Engine.get_architecture_name(),
		"editor_process": Engine.is_editor_hint(),
		"unsupported": [
			"alpha testing and transparent materials",
			"custom spatial shaders and double-sided materials",
			"procedural AABB geometry",
			"native denoising and shader execution reordering",
		],
	}
	var manifest_path := artifact_dir.path_join("e1_geometry_%s_manifest.json" % capture_label)
	var file := FileAccess.open(manifest_path, FileAccess.WRITE)
	if file == null:
		push_error("Failed to open C14 manifest: %s" % manifest_path)
		return
	file.store_string(JSON.stringify(manifest, "  ") + "\n")
