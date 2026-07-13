/**************************************************************************/
/*  capability_probe.mm                                                   */
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
/* "Software"), to deal in the Software without restriction, including    */
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

// Metal ray tracing capability probe (Metal RT port, chunk C2).
//
// Standalone tool: it links only Foundation and Metal, and is deliberately
// independent of the engine so it can run before an editor build exists.
// It emits one JSON capability record per the "GPU functional contract" in
// mac-rt-planning/testing-standard.md, and cross-checks measured device
// capabilities against the Apple GPU family expectations recorded in
// docs/rt_metal_port/capability_matrix.json.
//
// Build and run:
//   xcrun clang++ -std=c++17 -fobjc-arc -mmacosx-version-min=11.0 \
//     -framework Foundation -framework Metal \
//     docs/rt_metal_port/capability_probe.mm -o capability_probe
//   ./capability_probe [-o output.json]
//
// The canonical wrapper is the `caps` stage of
// mac-rt-planning/scripts/run_mac_rt_tests.py.
//
// Exit codes:
//   0: record written, all family-expectation checks passed.
//   1: probe error (could not query or serialize).
//   2: record written, at least one family-expectation check failed.
//   3: skipped; record contains "skip_reason" (e.g. no Metal device).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <sys/sysctl.h>

#include <cstdio>
#include <cstring>
#include <vector>

static const int PROBE_SCHEMA_VERSION = 1;

static NSString *sysctl_string(const char *p_name) {
	size_t size = 0;
	if (sysctlbyname(p_name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
		return nil;
	}
	std::vector<char> buffer(size);
	if (sysctlbyname(p_name, buffer.data(), &size, nullptr, 0) != 0) {
		return nil;
	}
	return [NSString stringWithUTF8String:buffer.data()];
}

static bool process_is_translated() {
	// Detects Rosetta 2: an x86_64 probe on Apple Silicon would otherwise
	// misreport the host architecture.
	int translated = 0;
	size_t size = sizeof(translated);
	if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) != 0) {
		return false;
	}
	return translated == 1;
}

static NSString *process_architecture() {
#if defined(__arm64__)
	return @"arm64";
#elif defined(__x86_64__)
	return @"x86_64";
#else
	return @"unknown";
#endif
}

struct FamilyProbe {
	const char *name;
	long raw_value;
};

// Raw values match Apple's MTLGPUFamily so families newer than this SDK can
// still be probed numerically (the engine uses the same technique in
// drivers/metal/metal_device_properties.cpp).
static const FamilyProbe FAMILY_PROBES[] = {
	{ "apple1", 1001 },
	{ "apple2", 1002 },
	{ "apple3", 1003 },
	{ "apple4", 1004 },
	{ "apple5", 1005 },
	{ "apple6", 1006 },
	{ "apple7", 1007 },
	{ "apple8", 1008 },
	{ "apple9", 1009 },
	{ "apple10", 1010 },
	{ "apple11", 1011 },
	{ "apple12", 1012 },
	{ "mac2", 2002 },
	{ "common1", 3001 },
	{ "common2", 3002 },
	{ "common3", 3003 },
	{ "metal3", 5001 },
	{ "metal4", 5002 },
};

static bool device_supports_family(id<MTLDevice> p_device, long p_raw_value) {
	return [p_device supportsFamily:(MTLGPUFamily)p_raw_value];
}

static NSDictionary *check_record(NSString *p_id, bool p_applicable, bool p_expected, bool p_actual, NSString *p_note) {
	return @{
		@"id" : p_id,
		@"applicable" : @(p_applicable),
		@"expected" : @(p_expected),
		@"actual" : @(p_actual),
		@"pass" : @(!p_applicable || p_expected == p_actual),
		@"note" : p_note,
	};
}

static NSDictionary *probe_device(id<MTLDevice> p_device, id<MTLDevice> p_default_device, NSOperatingSystemVersion p_os, bool *r_checks_failed) {
	NSMutableDictionary *record = [NSMutableDictionary dictionary];

	record[@"name"] = p_device.name;
	record[@"registry_id"] = @(p_device.registryID);
	record[@"is_default_device"] = @(p_default_device != nil && p_device.registryID == p_default_device.registryID);
	record[@"is_low_power"] = @(p_device.isLowPower);
	record[@"is_removable"] = @(p_device.isRemovable);
	record[@"has_unified_memory"] = @(p_device.hasUnifiedMemory);
	record[@"recommended_max_working_set_size"] = @(p_device.recommendedMaxWorkingSetSize);
	record[@"max_buffer_length"] = @(p_device.maxBufferLength);
	MTLSize threads = p_device.maxThreadsPerThreadgroup;
	record[@"max_threads_per_threadgroup"] = @{
		@"width" : @(threads.width),
		@"height" : @(threads.height),
		@"depth" : @(threads.depth),
	};

	// GPU families (raw supportsFamily sweep).
	NSMutableDictionary *families = [NSMutableDictionary dictionary];
	for (const FamilyProbe &probe : FAMILY_PROBES) {
		families[[NSString stringWithUTF8String:probe.name]] = @(device_supports_family(p_device, probe.raw_value));
	}
	record[@"families"] = families;

	long highest_apple_raw = 0;
	for (long raw = 1012; raw >= 1001; raw--) {
		if (device_supports_family(p_device, raw)) {
			highest_apple_raw = raw;
			break;
		}
	}
	record[@"highest_apple_family_raw"] = @(highest_apple_raw);

	bool apple6 = device_supports_family(p_device, 1006);
	bool apple8 = device_supports_family(p_device, 1008);
	bool apple9 = device_supports_family(p_device, 1009);
	bool mac2 = device_supports_family(p_device, 2002);
	bool metal3 = device_supports_family(p_device, 5001);

	// Ray-tracing capabilities. These runtime queries are the authority for
	// gating; family bits are only expectations (see capability_matrix.json).
	NSMutableDictionary *raytracing = [NSMutableDictionary dictionary];
	bool supports_raytracing = p_device.supportsRaytracing;
	bool supports_function_pointers = p_device.supportsFunctionPointers;
	raytracing[@"supports_raytracing"] = @(supports_raytracing);
	raytracing[@"supports_function_pointers"] = @(supports_function_pointers);
	raytracing[@"supports_primitive_motion_blur"] = @(p_device.supportsPrimitiveMotionBlur);
	bool supports_rt_from_render = false;
	if (@available(macOS 12.0, *)) {
		supports_rt_from_render = p_device.supportsRaytracingFromRender;
		raytracing[@"supports_raytracing_from_render"] = @(supports_rt_from_render);
		raytracing[@"supports_function_pointers_from_render"] = @(p_device.supportsFunctionPointersFromRender);
	} else {
		raytracing[@"supports_raytracing_from_render"] = [NSNull null];
		raytracing[@"supports_function_pointers_from_render"] = [NSNull null];
	}
	record[@"raytracing"] = raytracing;

	// Argument buffers.
	MTLArgumentBuffersTier tier = p_device.argumentBuffersSupport;
	record[@"argument_buffers_tier"] = tier == MTLArgumentBuffersTier2 ? @"tier2" : @"tier1";
	record[@"max_argument_buffer_sampler_count"] = @(p_device.maxArgumentBufferSamplerCount);

	// Default Metal Shading Language version for this OS/SDK combination,
	// decoded the same way as metal_device_properties.cpp.
	MTLCompileOptions *options = [MTLCompileOptions new];
	NSUInteger lang = (NSUInteger)options.languageVersion;
	NSUInteger msl_major = (lang >> 16) & 0xff;
	NSUInteger msl_minor = lang & 0xff;
	record[@"msl_max_version"] = [NSString stringWithFormat:@"%lu.%lu", (unsigned long)msl_major, (unsigned long)msl_minor];

	// Mirror of the derived feature logic in
	// drivers/metal/metal_device_properties.cpp (init_features), so the
	// record shows what the engine would conclude on this host.
	NSMutableDictionary *engine_view = [NSMutableDictionary dictionary];
	long engine_highest = highest_apple_raw > 1009 ? 1009 : highest_apple_raw; // Engine scan starts at Apple9.
	engine_view[@"highest_family"] = [NSString stringWithFormat:@"apple%ld", engine_highest - 1000];
	bool supports_gpu_address = false;
	if (@available(macOS 13.0, *)) {
		supports_gpu_address = true;
	}
	engine_view[@"supports_gpu_address"] = @(supports_gpu_address);
	bool needs_arg_encoders = true;
	if (@available(macOS 13.0, *)) {
		needs_arg_encoders = !(metal3 && tier == MTLArgumentBuffersTier2);
	}
	engine_view[@"needs_arg_encoders"] = @(needs_arg_encoders);
	engine_view[@"argument_buffers_supported"] = @(tier == MTLArgumentBuffersTier2 && !needs_arg_encoders);
	bool supports_native_image_atomics = false;
	if (@available(macOS 14.0, *)) {
		supports_native_image_atomics = (msl_major > 3 || (msl_major == 3 && msl_minor >= 1));
	}
	engine_view[@"supports_native_image_atomics"] = @(supports_native_image_atomics);
	bool supports_residency_sets = false;
	if (@available(macOS 15.0, *)) {
		supports_residency_sets = apple6;
	}
	engine_view[@"supports_residency_sets"] = @(supports_residency_sets);
	engine_view[@"supports_image_atomic_32_bit"] = @(apple6);
	engine_view[@"supports_image_atomic_64_bit"] = @(apple9 || (apple8 && mac2));
	record[@"engine_view"] = engine_view;

	// Cross-checks against the family expectations in capability_matrix.json.
	bool os12 = p_os.majorVersion >= 12;
	NSArray *checks = @[
		check_record(@"apple6_implies_raytracing", apple6, true, supports_raytracing,
				@"Apple feature tables list ray tracing from GPU family Apple6."),
		check_record(@"apple6_implies_function_pointers", apple6, true, supports_function_pointers,
				@"Function pointers (visible function tables) are expected from Apple6."),
		check_record(@"apple6_implies_argument_buffers_tier2", apple6, true, tier == MTLArgumentBuffersTier2,
				@"Apple silicon GPUs are expected to report argument buffers tier 2."),
		check_record(@"apple6_implies_unified_memory", apple6, true, p_device.hasUnifiedMemory,
				@"Apple silicon GPUs are expected to report unified memory."),
		check_record(@"apple6_os12_implies_raytracing_from_render", apple6 && os12, true, supports_rt_from_render,
				@"Ray tracing from render pipelines is expected on Apple6+ from macOS 12."),
	];
	record[@"checks"] = checks;
	for (NSDictionary *check in checks) {
		if (![check[@"pass"] boolValue]) {
			*r_checks_failed = true;
		}
	}

	return record;
}

int main(int argc, const char **argv) {
	@autoreleasepool {
		const char *output_path = nullptr;
		for (int i = 1; i < argc; i++) {
			if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
				output_path = argv[++i];
			} else {
				fprintf(stderr, "Usage: %s [-o output.json]\n", argv[0]);
				return 1;
			}
		}

		NSMutableDictionary *root = [NSMutableDictionary dictionary];
		root[@"record_type"] = @"metal_rt_capability_record";
		root[@"schema_version"] = @(PROBE_SCHEMA_VERSION);
		root[@"chunk"] = @"C2";

		NSDateFormatter *formatter = [NSDateFormatter new];
		formatter.dateFormat = @"yyyy-MM-dd'T'HH:mm:ss'Z'";
		formatter.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
		root[@"generated_at_utc"] = [formatter stringFromDate:[NSDate date]];

		NSOperatingSystemVersion os = [NSProcessInfo processInfo].operatingSystemVersion;
		NSString *os_build = sysctl_string("kern.osversion");
		NSString *model = sysctl_string("hw.model");
		root[@"host"] = @{
			@"os_product_version" : [NSString stringWithFormat:@"%ld.%ld.%ld", (long)os.majorVersion, (long)os.minorVersion, (long)os.patchVersion],
			@"os_build" : os_build ? os_build : @"unknown",
			@"model" : model ? model : @"unknown",
			@"process_architecture" : process_architecture(),
			@"process_translated" : @(process_is_translated()),
		};

		// The engine only enables the Metal rendering driver on arm64 builds
		// (platform/macos/detect.py), independent of what Metal itself reports.
		bool engine_arch_supported = [process_architecture() isEqualToString:@"arm64"] && !process_is_translated();
		root[@"engine_metal_driver_enabled_for_arch"] = @(engine_arch_supported);

		NSMutableDictionary *environment = [NSMutableDictionary dictionary];
		for (NSString *name in @[ @"MTL_DEBUG_LAYER", @"GODOT_MTL_TARGET_VERSION", @"GODOT_MTL_DISABLE_ARGUMENT_BUFFERS", @"GODOT_MTL_DISABLE_IMAGE_ATOMICS" ]) {
			NSString *value = [NSProcessInfo processInfo].environment[name];
			if (value != nil) {
				environment[name] = value;
			}
		}
		root[@"environment_overrides"] = environment;
		root[@"metal_api_validation_enabled"] = @([environment[@"MTL_DEBUG_LAYER"] isEqualToString:@"1"]);

		bool checks_failed = false;
		NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
		if (devices.count == 0) {
			root[@"skip_reason"] = @"runner_not_configured";
			root[@"devices"] = @[];
		} else {
			id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
			NSMutableArray *device_records = [NSMutableArray array];
			for (id<MTLDevice> device in devices) {
				[device_records addObject:probe_device(device, default_device, os, &checks_failed)];
			}
			root[@"devices"] = device_records;
		}

		NSError *error = nil;
		NSData *json = [NSJSONSerialization dataWithJSONObject:root
													   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
														 error:&error];
		if (json == nil) {
			fprintf(stderr, "ERROR: failed to serialize capability record: %s\n", error.localizedDescription.UTF8String);
			return 1;
		}

		if (output_path != nullptr) {
			if (![json writeToFile:[NSString stringWithUTF8String:output_path] atomically:YES]) {
				fprintf(stderr, "ERROR: failed to write %s\n", output_path);
				return 1;
			}
			fprintf(stderr, "Capability record written to %s\n", output_path);
		} else {
			fwrite(json.bytes, 1, json.length, stdout);
			fputc('\n', stdout);
		}

		if (devices.count == 0) {
			fprintf(stderr, "SKIP: no Metal device available (runner_not_configured)\n");
			return 3;
		}
		if (checks_failed) {
			fprintf(stderr, "FAIL: at least one family-expectation check failed\n");
			return 2;
		}
		fprintf(stderr, "OK: %lu device(s) probed, all family-expectation checks passed\n", (unsigned long)devices.count);
		return 0;
	}
}
