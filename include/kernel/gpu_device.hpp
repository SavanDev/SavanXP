#pragma once

namespace gpu_device {

// Registra /dev/gpu0 con un dispatcher de ioctl agnostico del backend: hace todo
// el marshalling de usuario y la gestion de sesion grafica, delegando la operacion
// concreta de hardware al backend activo via namespace display. Se llama siempre en
// el boot, exista o no virtio-gpu, asi el compositor puede abrir /dev/gpu0 sobre
// cualquier backend (virtio acelerado o framebuffer plano).
bool initialize();

} // namespace gpu_device
