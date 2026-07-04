// SavanXP - programa Haxe de validacion del subsistema nativo (Fase 2).
//
// Ejercita el contrato ABI v1 completo:
//  - El runtime (sx_entry.cpp) ya valido SXN_SYS_INFO antes de llegar aca.
//  - Clase Haxe con semantica por defecto: reflaxe.CPP genera
//    std::shared_ptr/make_shared, resueltos por el mini <memory> freestanding
//    del SDK sobre el heap nativo (sxn_alloc).
//  - Clase @:valueType: se genera como valor plano, sin heap.
//  - SXN_SYS_LOG via los helpers sxn_log/sxn_log_num.
//
// Los strings C se embeben dentro del template de __cpp__ (comillas simples
// afuera, dobles adentro) para no depender del String de Haxe todavia: el
// override _std de reflaxe.CPP queda para la proxima fase.

class Contador {
  public var valor:Int;

  public function new(inicial:Int) {
    this.valor = inicial;
  }

  public function incrementar():Int {
    this.valor += 1;
    return this.valor;
  }
}

@:valueType
class Punto {
  public var x:Int;
  public var y:Int;

  public function new(x:Int, y:Int) {
    this.x = x;
    this.y = y;
  }

  public function suma():Int {
    return x + y;
  }
}

function main() {
  untyped __cpp__("sxn_hello()");

  var contador = new Contador(0);
  contador.incrementar();
  contador.incrementar();
  var conteo = contador.incrementar();

  var punto = new Punto(3, 4);
  var suma = punto.suma();

  untyped __cpp__('sxn_log_num("main: contador (clase heap)", {0})', conteo);
  untyped __cpp__('sxn_log_num("main: punto.suma (valueType)", {0})', suma);

  if (conteo != 3 || suma != 7) {
    untyped __cpp__('sxn_log("main: resultados incorrectos")');
    untyped __cpp__("sxn_exit(1)");
  }

  untyped __cpp__('sxn_log("main: clases Haxe sobre heap nativo OK")');

  // _std habilitado: String y Array reales de Haxe (std::string / std::deque
  // sobre los mini headers freestanding del SDK).
  var partes = ["haxe", "nativo", "en"];
  partes.push("savanxp");

  var frase = "";
  for (parte in partes) {
    frase += parte + " ";
  }
  var mayusculas = frase.toUpperCase();

  var numeros = [10, 20, 30, 40];
  var total = 0;
  for (n in numeros) {
    total += n;
  }

  untyped __cpp__('sxn_log({0}.c_str())', mayusculas);
  untyped __cpp__('sxn_log_num("main: partes del array", {0})', partes.length);
  untyped __cpp__('sxn_log_num("main: suma del array", {0})', total);
  untyped __cpp__('sxn_log_num("main: largo de la frase", {0})', frase.length);
  untyped __cpp__('sxn_log("main: String/Array de Haxe OK")');
}
