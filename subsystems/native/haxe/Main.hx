// SavanXP - programa Haxe de validacion del subsistema nativo (Fase 0).
//
// Objetivo del "puntapie": probar la cadena completa
//   Haxe -> reflaxe.CPP -> C++17 -> clang++ freestanding -> ELF nativo de SavanXP.
//
// Llamamos a sxn_hello(), un primitivo del runtime nativo (sx_native.c), via
// inyeccion de C++ cruda. Usar el primitivo en C (en vez de trace()) mantiene
// el C++ generado SIN dependencias de libstdc++ (<iostream>/<string>), que es
// lo que permite compilar en freestanding. El header con la declaracion se
// fuerza con -include savanxp_native.h desde build.ps1.
function main() {
  untyped __cpp__("sxn_hello()");
}
