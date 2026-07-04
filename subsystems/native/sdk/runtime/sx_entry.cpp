/*
 * SavanXP - shim de entrada del subsistema nativo, Fase 2.
 *
 * reflaxe.CPP genera su propio `_main_.cpp` que asume libstdc++; lo excluimos
 * del build y proveemos esta entrada. crt0.S llama a `main`, y al retornar
 * dispara la syscall EXIT con el codigo de retorno.
 *
 * Antes de ceder el control al main generado por Haxe, el runtime valida el
 * contrato ABI contra el kernel (SXN_SYS_INFO): si la version no coincide, el
 * proceso aborta con codigo 132. La VM HashLink hara exactamente este mismo
 * handshake en su boot.
 */
#include "savanxp_native.h"

#include "Main.h"

extern "C" int main() {
    sxn_native_info info = {};
    if (sxn_info(&info) != 0 || info.abi_version != SXN_ABI_VERSION) {
        sxn_log("runtime: ABI del kernel incompatible");
        sxn_exit(132);
    }
    sxn_log_num("runtime: abi verificado, version", (long)info.abi_version);

    _Main::Main_Fields_::main();
    return 0;
}
