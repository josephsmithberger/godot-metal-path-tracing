/**************************************************************************/
/*  test_environment.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including  */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_environment)

#ifndef _3D_DISABLED

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"
#include "tests/test_utils.h"

namespace TestEnvironment {

TEST_CASE("[MetalRT] path-tracing denoisers use capability-safe values and hints") {
	Ref<Environment> environment;
	environment.instantiate();
	CHECK(environment->get_pathtracing_denoiser() == RSE::PT_DENOISER_NONE);

	bool default_is_valid = false;
	const Variant default_value = ClassDB::class_get_default_property_value("Environment", "pathtracing_denoiser", &default_is_valid);
	CHECK(default_is_valid);
	CHECK(int(default_value) == int(RSE::PT_DENOISER_NONE));

	CHECK(Environment::get_pathtracing_denoiser_property_hint(false, false) == "None:0");
	CHECK(Environment::get_pathtracing_denoiser_property_hint(true, false) == "None:0,DLSS Ray Reconstruction:1");
	CHECK(Environment::get_pathtracing_denoiser_property_hint(false, true) == "None:0,MetalFX Denoised Upscaling:2");
	CHECK(Environment::get_pathtracing_denoiser_property_hint(true, true) == "None:0,DLSS Ray Reconstruction:1,MetalFX Denoised Upscaling:2");
	CHECK(Environment::sanitize_pathtracing_denoiser(RSE::PT_DENOISER_NONE, false, false) == RSE::PT_DENOISER_NONE);
	CHECK(Environment::sanitize_pathtracing_denoiser(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION, false, false) == RSE::PT_DENOISER_NONE);
	CHECK(Environment::sanitize_pathtracing_denoiser(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION, true, false) == RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);
	CHECK(Environment::sanitize_pathtracing_denoiser(RSE::PT_DENOISER_METALFX, false, false) == RSE::PT_DENOISER_NONE);
	CHECK(Environment::sanitize_pathtracing_denoiser(RSE::PT_DENOISER_METALFX, false, true) == RSE::PT_DENOISER_METALFX);

	List<PropertyInfo> property_list;
	environment->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (property.name == "pathtracing_denoiser") {
			const bool dlss_rr_supported = RenderingServer::get_singleton()->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);
			const bool metalfx_supported = RenderingServer::get_singleton()->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_METALFX);
			CHECK(property.hint_string == Environment::get_pathtracing_denoiser_property_hint(dlss_rr_supported, metalfx_supported));
			return;
		}
	}
	FAIL_CHECK("The Environment pathtracing_denoiser property was not registered.");
}

TEST_CASE("[MetalRT] C17 serializes the deterministic None denoiser") {
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_pathtracing_enabled(true);
	environment->set_pathtracing_denoiser(RSE::PT_DENOISER_NONE);

	const String save_path = TestUtils::get_temp_path("c17_environment.tres");
	REQUIRE(ResourceSaver::save(environment, save_path) == OK);
	Ref<Environment> loaded = ResourceLoader::load(save_path, "Environment", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_pathtracing_denoiser() == RSE::PT_DENOISER_NONE);
}

TEST_CASE("[MetalRT] Metal exposes only supported native path-tracing denoisers") {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);
	CHECK(rendering_server->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_NONE));
	if (rendering_server->get_current_rendering_driver_name() == "metal") {
		CHECK_FALSE(rendering_server->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION));

		Ref<Environment> environment;
		environment.instantiate();
		environment->set_pathtracing_denoiser(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);
		CHECK(environment->get_pathtracing_denoiser() == RSE::PT_DENOISER_NONE);

		const bool metalfx_supported = rendering_server->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_METALFX);
		environment->set_pathtracing_denoiser(RSE::PT_DENOISER_METALFX);
		CHECK(environment->get_pathtracing_denoiser() == (metalfx_supported ? RSE::PT_DENOISER_METALFX : RSE::PT_DENOISER_NONE));
	}
}

} // namespace TestEnvironment

#endif // _3D_DISABLED
