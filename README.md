# SavanXP

Experimento de hobby OS en `C++` sobre `x86_64 + UEFI`, con `Limine` como
bootloader y flujo de trabajo pensado para Windows nativo.

## Estado actual

El primer hito deja armado:

- Kernel `freestanding` en C++ con ABI de entrada propio.
- Integracion con Limine para recibir `bootloader info`, `firmware type`,
  `framebuffer`, `memory map` y `HHDM`.
- Consola temprana por serie (`COM1`) con salida de depuracion.
- Pantalla de bienvenida sobre framebuffer con resumen visual del progreso.
- GDT/IDT basicas con manejo minimo de excepciones y self-test por `int3`.
- Allocador fisico temprano sobre regiones `usable` del mapa de memoria.
- Heap temprano del kernel sobre paginas del PMM.
- Cableado de IRQs listo y timer basado en `local APIC/x2APIC` con probe de
  interrupcion por vector dedicado.
- Script `build.ps1` con comandos `build`, `run`, `debug` y `clean`.

## Prerrequisitos

- `git`
- `clang++`
- `ld.lld` o `clang++` con `-fuse-ld=lld`
- `qemu-system-x86_64`
- Firmware OVMF accesible por una de estas vias:
  - variables de entorno `OVMF_CODE` y `OVMF_VARS`
  - o una instalacion de QEMU/MSYS2 en una ruta comun

`build.ps1` descarga automaticamente la rama binaria `v10.x-binary` de Limine
si no existe en `tools/limine`.

## Uso

```powershell
.\build.ps1 build
.\build.ps1 run
.\build.ps1 debug
.\build.ps1 clean
```

Durante `run` y `debug`, QEMU expone la salida serie en la terminal. El kernel
imprime un banner, datos de firmware/bootloader y un resumen del mapa de
memoria.
