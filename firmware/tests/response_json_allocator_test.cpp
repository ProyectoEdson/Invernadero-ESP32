#define ARDUINOJSON_POOL_CAPACITY 32
#include "../invernadero/response_json_allocator.h"
#include <cassert>
#include <cstdio>

int main() {
  {
    char response[] = R"({"ok":true,"accepted":false,"command":null})";
    ResponseJsonAllocator allocator;
    JsonDocument document(&allocator);
    const auto error = deserializeJson(document, response, sizeof(response) - 1,
                                        DeserializationOption::NestingLimit(2));
    assert(!error && !document.overflowed());
    assert(document["ok"].is<bool>() && document["ok"].as<bool>());
    assert(document["accepted"].is<bool>() && !document["accepted"].as<bool>());
    assert(document["command"].isNull());
  }
  {
    char response[] = R"({"ok":true,"accepted":true,"command":{"id":"12345678-1234-4234-8234-123456789abc","sequence":9007199254740991,"kind":"pump_pulse","value":null,"duration_ms":3000,"expires_in_ms":14999,"target_boot_id":"0123456789abcdef0123456789abcdef"}})";
    ResponseJsonAllocator allocator;
    JsonDocument document(&allocator);
    const auto error = deserializeJson(document, response, sizeof(response) - 1,
                                        DeserializationOption::NestingLimit(2));
    assert(!error && !document.overflowed());
    assert(document["command"]["sequence"].as<uint64_t>() == 9007199254740991ULL);
    assert(document["command"]["duration_ms"].as<unsigned>() == 3000);
    assert(std::strcmp(document["command"]["id"].as<const char *>(),
                       "12345678-1234-4234-8234-123456789abc") == 0);
    assert(std::strcmp(document["command"]["target_boot_id"].as<const char *>(),
                       "0123456789abcdef0123456789abcdef") == 0);
  }
  std::puts("OK: respuesta sin orden y respuesta con comando completo, sin NoMemory.");
}
