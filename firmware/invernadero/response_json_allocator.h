#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Arena por respuesta: ArduinoJson necesita varios bloques para strings y slots.
// Ningún bloque sale de este documento; toda la arena se libera al salir del scope.
class ResponseJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override {
    const size_t aligned = alignedSize(size);
    if (size == 0 || aligned > sizeof(storage) - used ||
        sizeof(Block) > sizeof(storage) - used - aligned) {
      return nullptr;
    }
    Block *block = new (storage + used) Block{size, used + sizeof(Block) + aligned};
    used = block->end;
    return block + 1;
  }

  void deallocate(void *pointer) override {
    if (pointer == nullptr) return;
    Block *block = static_cast<Block *>(pointer) - 1;
    if (block->end == used) {
      used = size_t(reinterpret_cast<uint8_t *>(block) - storage);
    }
  }

  void *reallocate(void *pointer, size_t size) override {
    if (pointer == nullptr) return allocate(size);
    if (size == 0) {
      deallocate(pointer);
      return nullptr;
    }
    Block *block = static_cast<Block *>(pointer) - 1;
    if (size <= block->size) return pointer;
    const size_t start = size_t(reinterpret_cast<uint8_t *>(block) - storage);
    const size_t aligned = alignedSize(size);
    if (block->end == used && aligned <= sizeof(storage) - start - sizeof(Block)) {
      used = start + sizeof(Block) + aligned;
      block->size = size;
      block->end = used;
      return pointer;
    }
    void *replacement = allocate(size);
    if (replacement != nullptr) std::memcpy(replacement, pointer, block->size);
    return replacement;
  }

 private:
  struct alignas(max_align_t) Block {
    size_t size;
    size_t end;
  };
  static size_t alignedSize(size_t size) {
    constexpr size_t alignment = alignof(max_align_t);
    if (size > SIZE_MAX - (alignment - 1)) return SIZE_MAX;
    return (size + alignment - 1) & ~(alignment - 1);
  }
  alignas(max_align_t) uint8_t storage[2048];
  size_t used = 0;
};
