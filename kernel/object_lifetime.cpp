#include "kernel/object.hpp"

namespace object {

void retain(Header* object) {
    if (object != nullptr) {
        object->refcount += 1;
    }
}

void release(Header*& object) {
    Header* current = object;
    object = nullptr;
    if (current == nullptr) {
        return;
    }

    if (current->refcount != 0) {
        current->refcount -= 1;
    }
    if (current->refcount == 0 && current->destroy != nullptr) {
        current->destroy(current);
    }
}

IoObject* as_io(Header* object) {
    if (object == nullptr || object->type != Type::io) {
        return nullptr;
    }
    return reinterpret_cast<IoObject*>(object);
}

const IoObject* as_io(const Header* object) {
    if (object == nullptr || object->type != Type::io) {
        return nullptr;
    }
    return reinterpret_cast<const IoObject*>(object);
}

} // namespace object
