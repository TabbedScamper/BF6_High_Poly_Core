#include "bf6_core.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

int main() {
    const float values[6] = {1.25f, -2.5f, 17.f, -0.f, 8.f, 99.f};
    std::array<uint8_t, sizeof(values)> bytes{};
    std::memcpy(bytes.data(), values, sizeof(values));
    std::array<float, 6> output{};
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    check(bf6_decode_vertex_attribute(bytes.data(), bytes.size(), 0, 12, 2, 3, nullptr, 0) == 3, "component query");
    check(bf6_decode_vertex_attribute(bytes.data(), bytes.size(), 0, 12, 2, 3, output.data(), 6) == 3
          && std::memcmp(output.data(), values, sizeof(values)) == 0, "float3 exact bytes");
    struct Bad { int64_t len, first, stride; int count, format; int64_t capacity; };
    const Bad bad[] = {
        {24, 0, 12, 2, 3, 5}, {23, 0, 12, 2, 3, 6},
        {24, -1, 12, 2, 3, 6}, {24, 0, 0, 2, 3, 6},
        {24, 0, 12, -1, 3, 6}, {24, 0, 12, 2, 999, 6},
        {24, INT64_MAX, 12, 2, 3, 6}, {24, 0, INT64_MAX, 2, 3, 6},
        {-1, 0, 12, 2, 3, 6}, {24, 0, 12, 2, 3, -1}
    };
    for (const auto& row : bad) {
        output.fill(1234.f);
        const auto before = output;
        check(bf6_decode_vertex_attribute(bytes.data(), row.len, row.first, row.stride,
              row.count, row.format, output.data(), row.capacity) == 0, "invalid call rejected");
        check(std::memcmp(before.data(), output.data(), sizeof(output)) == 0, "invalid call wrote nothing");
    }
    check(bf6_decode_vertex_attribute(nullptr, 24, 0, 12, 2, 3, output.data(), 6) == 0, "null input");
    std::atomic<int> concurrent_failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&]() {
        std::array<float, 6> private_output{};
        const auto private_input = bytes;
        for (int n = 0; n < 10000; ++n) {
            if (bf6_decode_vertex_attribute(private_input.data(), private_input.size(), 0, 12, 2, 3,
                private_output.data(), private_output.size()) != 3 ||
                std::memcmp(private_output.data(), values, sizeof(values)) != 0) ++concurrent_failures;
        }
    });
    for (auto& worker : workers) worker.join();
    check(concurrent_failures == 0, "80000 concurrent calls on independent buffers");
    std::printf("vertex_attribute_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
