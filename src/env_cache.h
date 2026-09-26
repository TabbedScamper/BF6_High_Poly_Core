#pragma once
/* ENVIRONMENT SWITCHES, READ ONCE.
 *
 * The expression VM and the vehicle hosts check dozens of BF6_* debug switches, many
 * of them per record, per operator call or per wheel. std::getenv on Windows takes
 * the CRT environment lock and scans the whole block every call, and at a few
 * thousand records a tick that was a measurable share of a vehicle step. The switches
 * are set before a run and never change during one, so each name is looked up once
 * and the answer kept (the pointer the CRT returned stays valid while the variable
 * is not modified).
 *
 * Included by the hot translation units, which then call bf6_env instead of
 * std::getenv. */
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

inline const char* bf6_env(const char* name) {
    static std::mutex mutex;
    static std::unordered_map<std::string, const char*> cache;
    std::lock_guard<std::mutex> guard(mutex);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    const char* v = std::getenv(name);
    cache.emplace(name, v);
    return v;
}
