#include "kernel/kernel.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/ac97.hpp"
#include "kernel/acpi.hpp"
#include "kernel/audio.hpp"
#include "kernel/ata.hpp"
#include "kernel/audio_device.hpp"
#include "kernel/block.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/fb_gpu.hpp"
#include "kernel/fs.hpp"
#include "kernel/gpu_device.hpp"
#include "kernel/heap.hpp"
#include "kernel/input.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/net.hpp"
#include "kernel/nic.hpp"
#include "kernel/panic.hpp"
#include "kernel/partition.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/clipboard.hpp"
#include "kernel/power.hpp"
#include "kernel/process.hpp"
#include "kernel/ps2.hpp"
#include "kernel/ramdisk.hpp"
#include "kernel/rtl8139.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/uacpi_glue.hpp"
#include "kernel/ui.hpp"
#include "kernel/virtio_gpu.hpp"
#include "kernel/virtio_input.hpp"
#include "kernel/virtio_sound.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "shared/version.h"

namespace
{

    const char *firmware_type_name(boot::FirmwareType firmware_type)
    {
        switch (firmware_type)
        {
        case boot::FirmwareType::x86_bios:
            return "x86 BIOS";
        case boot::FirmwareType::efi32:
            return "UEFI 32";
        case boot::FirmwareType::efi64:
            return "UEFI 64";
        case boot::FirmwareType::sbi:
            return "SBI";
        default:
            return "unknown";
        }
    }

    uint64_t mib_from_bytes(uint64_t value)
    {
        return value / (1024ULL * 1024ULL);
    }

    void copy_string_field(char *destination, size_t capacity, const char *source)
    {
        if (destination == nullptr || capacity == 0)
        {
            return;
        }

        size_t index = 0;
        while (source != nullptr && source[index] != '\0' && index + 1 < capacity)
        {
            destination[index] = source[index];
            ++index;
        }
        destination[index] = '\0';
    }

    struct MemorySummary
    {
        uint64_t usable_bytes;
        uint64_t reclaimable_bytes;
    };

    MemorySummary summarize_memory(const boot::BootInfo &boot_info)
    {
        MemorySummary summary = {};
        for (size_t index = 0; index < boot_info.memory_map_entries; ++index)
        {
            const boot::MemoryRegion &entry = boot_info.memory_map[index];
            if (entry.type == boot::MemoryRegionType::usable)
            {
                summary.usable_bytes += entry.length;
            }
            else if (entry.type == boot::MemoryRegionType::acpi_reclaimable ||
                     entry.type == boot::MemoryRegionType::bootloader_reclaimable)
            {
                summary.reclaimable_bytes += entry.length;
            }
        }
        return summary;
    }

} // namespace

[[noreturn]] void kernel_main(const boot::BootInfo &boot_info)
{
    arch::x86_64::initialize_cpu();

    boot_screen::initialize(boot_info.framebuffer);
    if (boot_screen::ready())
    {
        console::set_framebuffer_console_enabled(false);
        boot_screen::show(4, "Preparando CPU");
    }

    console::printf("%s booting...\n", SAVANXP_DISPLAY_NAME);

    boot_screen::show(12, "Inicializando memoria fisica");
    memory::initialize(boot_info);
    if (!memory::ready())
    {
        panic("pmm: no usable memory");
    }

    boot_screen::show(20, "Inicializando heap");
    heap::initialize();
    if (!heap::ready())
    {
        panic("heap: bootstrap failed");
    }

    boot_screen::show(28, "Activando memoria virtual");
    vm::initialize(boot_info);
    if (!vm::ready())
    {
        panic("vmm: bootstrap failed");
    }

    boot_screen::show(36, "Detectando firmware");
    acpi::initialize(boot_info);

    tty::initialize();
    input::initialize();
    timer::initialize(1000);
    // IOAPIC despues del Local APIC (lo levanta timer::initialize) y de la ACPI:
    // parsea la MADT y deja el ruteo de GSIs listo para SCI e INTx (_PRT). Si no
    // hay MADT/IOAPIC cae en silencio y el sistema sigue con el PIC legacy.
    ioapic::initialize(boot_info.acpi_rsdp_address, boot_info.hhdm_offset);
    // Con el IOAPIC listo, habilitar el modo ACPI y rutear la SCI (boton de power).
    acpi::start_sci();
    // Traer uACPI hasta cargar/inicializar el namespace (interpreta el AML de la
    // DSDT). Todavia no toma los eventos: convive con acpi::start_sci.
    uacpi_glue::bringup(boot_info.acpi_rsdp_address, boot_info.hhdm_offset);
    uacpi_glue::dump_pci_routing();
    pci::initialize();
    boot_screen::show(46, "Inicializando entrada");
    virtio_input::initialize(boot_info.framebuffer);
    ps2::initialize();
    process::initialize();
    if (subsystem::dispatcher_for(subsystem::Id::posix) == nullptr ||
        subsystem::dispatcher_for(subsystem::Id::native) == nullptr ||
        subsystem::dispatcher_for(subsystem::Id::posix) ==
            subsystem::dispatcher_for(subsystem::Id::native))
    {
        panic("subsystem: registro de dispatch incompleto");
    }
    boot_screen::show(56, "Cargando userland");
    vfs::initialize(boot_info.initramfs_address, static_cast<size_t>(boot_info.initramfs_size));
    device::initialize();
    boot_screen::show(68, "Preparando display");
    // Elegir el backend de display: cada driver se registra y bind_best corre
    // sus probes por prioridad, quedandose con el primero que reclame el
    // hardware (virtio-gpu si el probe PCI lo encontro, si no el framebuffer
    // plano sobre el scanout lineal de Limine, caso VirtualBox/VGA). Sumar un
    // backend nuevo es una linea mas aca y no una rama nueva. El nodo /dev/gpu0
    // lo registra gpu_device sobre el backend elegido.
    display::register_driver(virtio_gpu::driver());
    display::register_driver(fb_gpu::driver());
    if (const display::Driver* bound = display::bind_best(boot_info.framebuffer))
    {
        console::printf("display: backend '%s'\n", bound->name);
    }
    else
    {
        console::printf("display: ningun driver reclamo el hardware\n");
    }
    gpu_device::initialize();
    ui::initialize(boot_info.framebuffer);
    boot_screen::show(80, "Inicializando dispositivos");
    pcspeaker::initialize();
    power::initialize();
    clipboard::initialize();
    // Backend de audio: mismo registro por prioridad que display. virtio-sound
    // gana si el probe PCI lo encontro; si no, AC97 (el caso VirtualBox).
    // audio_device registra /dev/audio0 sobre el backend elegido, o no lo
    // registra si ningun driver reclamo hardware de sonido.
    audio::register_driver(virtio_sound::driver());
    audio::register_driver(ac97::driver());
    if (const audio::Driver* bound = audio::bind_best())
    {
        console::printf("audio: backend '%s'\n", bound->name);
    }
    else
    {
        console::printf("audio: ningun driver reclamo hardware de sonido\n");
    }
    audio_device::initialize();
    // NIC: mismo registro por prioridad que display y audio. El probe deja el
    // device reconocido y le rutea la INTx, pero no lo levanta: la subida real
    // la pide net:: cuando alguien hace NET_IOC_UP sobre /dev/net0.
    nic::register_driver(rtl8139::driver());
    if (const nic::Driver* bound = nic::bind_best())
    {
        console::printf("nic: driver '%s'\n", bound->name);
    }
    else
    {
        console::write("nic: ningun driver reclamo el hardware\n");
    }
    net::initialize();
    // Almacenamiento: mismo registro de drivers que display y audio, pero aca
    // los devices de todos COEXISTEN, asi que probe_all corre todos los
    // enumerate en vez de cortar en el primero. La prioridad define el orden de
    // los indices: los ATA enumeran antes que el ramdisk, asi un disco IDE
    // persistente (dev) tiene prioridad y en la ISO pura montamos el ramdisk.
    // Cual de estos devices termina siendo la raiz lo decide fs::mount_any mas
    // abajo, recorriendolos en este mismo orden.
    block::register_driver(ata::driver());
    block::register_driver(ramdisk::driver());
    // Ultimo en la fila (prioridad negativa): rebana en devices propios
    // las particiones MBR/GPT de los discos que enumeraron los de arriba.
    block::register_driver(partition::driver());
    // LiveCD: si Limine cargo la imagen de disco como modulo, ofrecersela al
    // driver de ramdisk antes de enumerar.
    //
    // Escribible-efimero: las apps que crean directorios/archivos en /disk
    // (p.ej. doom prepara /disk/games/doom/savegames) necesitan un FS de
    // lectura-escritura. Escribimos in-place sobre la memoria del modulo, que es
    // RAM normal mapeada RW por el HHDM de Limine (el kernel reutiliza su CR3);
    // los cambios viven solo durante la sesion y se pierden al reiniciar, que es
    // la semantica correcta de un LiveCD. La persistencia real llega con la
    // instalacion a disco.
    if (boot_info.disk_image_address != nullptr && boot_info.disk_image_size != 0)
    {
        ramdisk::attach_image(
            const_cast<void *>(boot_info.disk_image_address),
            boot_info.disk_image_size,
            /*writable=*/true,
            "livecd");
    }
    const size_t block_devices = block::probe_all();
    console::printf("block: %u device(s)", static_cast<unsigned>(block_devices));
    for (size_t index = 0; index < block_devices; ++index)
    {
        block::DeviceInfo device_info = {};
        if (block::device_info(index, device_info))
        {
            console::printf(
                " %s(%s)", device_info.name, device_info.writable ? "rw" : "ro");
        }
    }
    console::write("\n");
    boot_screen::show(90, "Montando almacenamiento");
    // Registro de sistemas de archivos: fs:: decide QUE device se monta y
    // DONDE, y cada driver solo dice si reconoce el formato. mount_any recorre
    // los block devices (particiones incluidas) y se queda con el primero que
    // alguien reclame para la raiz del disco.
    fs::initialize();
    sxfs::initialize();
    fs::register_driver(sxfs::driver());
    const size_t root_mount = fs::mount_any(sxfs::kRootMountPoint);

    if (!vfs::ready())
    {
        panic("vfs: initramfs unavailable");
    }
    // attach va aparte de mount porque la metadata se lee antes de que vfs::
    // este listo para recibir vnodes.
    const bool disk_mounted = fs::attach(root_mount);

    const MemorySummary memory = summarize_memory(boot_info);
    savanxp_system_info system_info = {};
    copy_string_field(system_info.bootloader_name, sizeof(system_info.bootloader_name), boot_info.bootloader_name);
    copy_string_field(system_info.bootloader_version, sizeof(system_info.bootloader_version), boot_info.bootloader_version);
    copy_string_field(system_info.firmware, sizeof(system_info.firmware), firmware_type_name(boot_info.firmware_type));
    system_info.input_ready = ps2::ready() ? 1u : 0u;
    system_info.framebuffer_ready = ui::framebuffer_available() ? 1u : 0u;
    system_info.net_present = net::present() ? 1u : 0u;
    system_info.speaker_ready = pcspeaker::ready() ? 1u : 0u;
    system_info.block_ready = block::ready() ? 1u : 0u;
    system_info.sxfs_mounted = disk_mounted ? 1u : 0u;
    switch (timer::backend())
    {
    case timer::Backend::local_apic:
        system_info.timer_backend = SAVANXP_TIMER_LOCAL_APIC;
        break;
    case timer::Backend::pit:
        system_info.timer_backend = SAVANXP_TIMER_PIT;
        break;
    case timer::Backend::none:
    default:
        system_info.timer_backend = SAVANXP_TIMER_NONE;
        break;
    }
    system_info.timer_frequency_hz = timer::frequency_hz();
    system_info.framebuffer_width = ui::framebuffer_info().width;
    system_info.framebuffer_height = ui::framebuffer_info().height;
    system_info.framebuffer_bpp = ui::framebuffer_info().bpp;
    system_info.pci_device_count = static_cast<uint32_t>(pci::device_count());
    system_info.sxfs_file_count = static_cast<uint32_t>(sxfs::file_count(sxfs::root()));
    system_info.memory_usable_bytes = memory.usable_bytes;
    system_info.memory_reclaimable_bytes = memory.reclaimable_bytes;
    system_info.memory_total_pages = memory::total_page_count();
    system_info.sxfs_total_bytes = sxfs::total_bytes(sxfs::root());
    system_info.sxfs_used_bytes = sxfs::used_bytes(sxfs::root());
    system_info.sxfs_free_bytes = sxfs::free_bytes(sxfs::root());
    system_info.initramfs_size = boot_info.initramfs_size;
    process::set_boot_system_info(system_info);

    console::printf(
        "boot ready: %u MiB usable, %u MiB reclaimable, video %ux%u\n",
        static_cast<unsigned>(mib_from_bytes(memory.usable_bytes)),
        static_cast<unsigned>(mib_from_bytes(memory.reclaimable_bytes)),
        system_info.framebuffer_width,
        system_info.framebuffer_height);
    console::printf(
        "handoff: starting /bin/init (%s, /disk %s)\n",
        system_info.net_present != 0 ? "net online" : "net absent",
        disk_mounted ? "mounted" : "offline");
    console::write_line("");

    boot_screen::show(100, "Iniciando bienvenida");
    process::start_init("/bin/init");
}
