/* fscheck -- informe de consistencia del volumen SxFS.
 *
 * Equivalente dentro del sistema de `sxfs-cli check`, que solo se puede ejecutar
 * en el host con la imagen desmontada. Este va contra el volumen montado y es de
 * SOLO LECTURA: no repara, no escribe y no puede empeorar un sistema de archivos
 * que ya este roto. Por eso el codigo de salida distingue "todo en orden" de
 * "encontramos algo", y no "repare" de "no pudo reparar": no hay reparacion.
 *
 * Lo que se ve aqui y no se ve con df es la reconciliacion. df resta used de
 * total sobre el bitmap, asi que un sector marcado ocupado que ningun archivo
 * reclama se le aparece al usuario como espacio perdido sin explicacion. Este
 * programa separa las dos mitades, occupied y claimed, y las facing.
 */

#include "libc.h"

static void print_u64(uint64_t value) {
    char buffer[32];
    int index = 0;

    if (value == 0) {
        putchar_fd(1, '0');
        return;
    }

    while (value != 0 && index < (int)sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        putchar_fd(1, buffer[--index]);
    }
}

static void print_padded(const char* text, int width) {
    int length = 0;
    while (text[length] != '\0') {
        putchar_fd(1, text[length]);
        ++length;
    }
    while (length < width) {
        putchar_fd(1, ' ');
        ++length;
    }
}

static void print_label(const char* label) {
    print_padded(label, 22);
}

static void print_u64_line(const char* label, uint64_t value) {
    print_label(label);
    print_u64(value);
    putchar_fd(1, '\n');
}

static void print_yes_no_line(const char* label, uint8_t value) {
    print_label(label);
    puts_out(value != 0 ? "yes" : "no");
    putchar_fd(1, '\n');
}

/* Devuelve 1 si el contador es distinto de cero, para decidir si el hallazgo
 * entra en el total. */
static int is_finding(uint32_t value) {
    return value != 0;
}

int main(void) {
    struct savanxp_fscheck_report report = {};
    const long status = fscheck(&report);
    if (status < 0) {
        eprintf("fscheck: fscheck failed (%s)\n", result_error_string(status));
        return 1;
    }

    puts_out("SxFS consistency report\n");

    puts_out("geometry:\n");
    print_u64_line("  total_sectors", report.total_sectors);
    print_u64_line("  data_lba", report.data_lba);
    print_u64_line("  data_sectors", report.data_sectors);
    print_u64_line("  superblock_seq", report.sequence);
    print_yes_no_line("  clean_shutdown", report.clean_shutdown);
    /* El journal se borra a cero cuando no hay transaccion a medias, y un header
     * vacio no tiene magia, asi que journal_valid = no es el estado normal de un
     * volumen sano: solo se marca cuando hay un header con magia que se pudo
     * interpretar. Por eso clean_shutdown es el que responde "quedo algo a
     * medias", y no journal_pending. */
    print_yes_no_line("  journal_valid", report.journal_valid);
    print_u64_line("  journal_pending", report.journal_pending);

    puts_out("population:\n");
    print_u64_line("  inodes_allocated", report.inodes_allocated);
    print_u64_line("  files", report.files);
    print_u64_line("  directories", report.directories);

    puts_out("block accounting:\n");
    print_u64_line("  data_used", report.data_used_sectors);
    print_u64_line("  data_claimed", report.data_claimed_sectors);

    /* Los dos numeros que de verdad importan, uno al lado del otro.
     * Iguales: el bitmap y los archivos coinciden. Distintos: hay diferencia y
     * los hallazgos de abajo dicen de que lado esta. */
    puts_out("reconciliation:\n");
    if (report.data_used_sectors == report.data_claimed_sectors) {
        puts_out("  used == claimed (balanced)\n");
    } else {
        puts_out("  used != claimed (unbalanced)\n");
    }

    uint32_t findings = 0;
    findings += is_finding(report.leaked_blocks);
    findings += is_finding(report.lost_blocks);
    findings += is_finding(report.double_claimed);
    findings += is_finding(report.metadata_unmarked);
    findings += is_finding(report.orphan_inodes);
    findings += is_finding(report.alias_inodes);
    findings += is_finding(report.duplicate_names);
    findings += is_finding(report.bad_dir_entries);
    findings += is_finding(report.unreadable_dirs);
    /* El journal solo aporta un hallazgo cuando hay un header CON magia que no
     * valida: un journal limpio esta a cero y no tiene magia, y eso es lo normal.
     * clean_shutdown tampoco cuenta: el superblock se marca sucio cuando el
     * montaje completo necesita escribir, o sea en cada arranque. */
    if (report.journal_valid == 0 && report.journal_pending != 0) {
        ++findings;
    }

    puts_out("findings:\n");
    if (findings == 0) {
        puts_out("  none\n");
        puts_out("\nresult: clean\n");
        return 0;
    }

    if (report.leaked_blocks != 0) {
        print_label("  leaked_blocks");
        print_u64(report.leaked_blocks);
        puts_out("  (occupied, owned by no inode)\n");
    }
    if (report.lost_blocks != 0) {
        print_label("  lost_blocks");
        print_u64(report.lost_blocks);
        puts_out("  (claimed by an inode, free in bitmap)\n");
    }
    if (report.double_claimed != 0) {
        print_label("  double_claimed");
        print_u64(report.double_claimed);
        puts_out("  (two inodes on one sector)\n");
    }
    if (report.metadata_unmarked != 0) {
        print_label("  metadata_unmarked");
        print_u64(report.metadata_unmarked);
        puts_out("  (metadata sector marked free)\n");
    }
    if (report.orphan_inodes != 0) {
        print_label("  orphan_inodes");
        print_u64(report.orphan_inodes);
        puts_out("  (allocated, unreachable from root)\n");
    }
    if (report.alias_inodes != 0) {
        print_label("  alias_inodes");
        print_u64(report.alias_inodes);
        puts_out("  (two entries for one inode)\n");
    }
    if (report.duplicate_names != 0) {
        print_label("  duplicate_names");
        print_u64(report.duplicate_names);
        puts_out("  (one name twice in a directory)\n");
    }
    if (report.bad_dir_entries != 0) {
        print_label("  bad_dir_entries");
        print_u64(report.bad_dir_entries);
        puts_out("  (entry failed validation)\n");
    }
    if (report.unreadable_dirs != 0) {
        print_label("  unreadable_dirs");
        print_u64(report.unreadable_dirs);
        puts_out("  (unreadable or too deep)\n");
    }
    if (report.journal_valid == 0 && report.journal_pending != 0) {
        puts_out("  journal_invalid      (header with magic failed validation)\n");
    }

    puts_out("\nresult: findings (report only, nothing was modified)\n");
    return 1;
}
