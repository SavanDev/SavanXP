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

// Lienzo de pixeles XRGB de 32 bits presentable via el ABI gfx nativo. El
// Array<Int> de Haxe mapea a nuestro std::deque freestanding, que ES contiguo
// (garantia de sdk/include/cxxstd/deque), asi que &(*pixeles)[0] es un frame
// valido para SXN_SYS_GFX_PRESENT.
class Lienzo {
  public var ancho:Int;
  public var alto:Int;
  public var pixeles:Array<Int>;

  public function new(ancho:Int, alto:Int) {
    this.ancho = ancho;
    this.alto = alto;
    this.pixeles = [];
    untyped __cpp__("{0}->resize((std::size_t)({1}))", pixeles, ancho * alto);
  }

  public function rectangulo(x0:Int, y0:Int, w:Int, h:Int, color:Int) {
    for (fila in y0...(y0 + h)) {
      for (columna in x0...(x0 + w)) {
        pixeles[fila * ancho + columna] = color;
      }
    }
  }

  public function degrade() {
    for (fila in 0...alto) {
      for (columna in 0...ancho) {
        // Division entera via C++: la division de Haxe es Float, y el runtime
        // nativo aun no soporta floats (freestanding con -mgeneral-regs-only y
        // sin compiler-rt: los intrinsics soft-float no existen). Deuda.
        var rojo:Int = untyped __cpp__("(({0} * 255) / {1})", columna, ancho);
        var azul:Int = untyped __cpp__("(({0} * 255) / {1})", fila, alto);
        pixeles[fila * ancho + columna] = (rojo << 16) | azul;
      }
    }
  }

  public function presentar():Int {
    return untyped __cpp__(
      "(int)sxn_gfx_present(&(*{0})[0], (unsigned)({1} * 4), 0, 0, (unsigned){1}, (unsigned){2})",
      pixeles, ancho, alto);
  }
}

// Null<T> real de Haxe: reflaxe.CPP lo mapea a std::optional<T>, resuelto por
// el mini <optional> freestanding del SDK. Sin division (Float sigue sin
// soporte): tabla chica a mano.
function buscarValor(clave:Int):Null<Int> {
  if (clave == 1) return 100;
  if (clave == 2) return 200;
  return null;
}

// Demo grafica: toma la sesion de display, dibuja un degrade con un rectangulo
// verde centrado y lo presenta. Si el display esta ocupado (compositor activo)
// la demo se omite sin fallar: la sesion exclusiva es de a un proceso por vez.
function demoGrafica() {
  untyped __cpp__("struct sxn_gfx_info __gfx; long __gfx_result = sxn_gfx_info(&__gfx)");
  if (untyped __cpp__("__gfx_result != 0")) {
    untyped __cpp__('sxn_log("gfx: sin display (ENODEV), demo omitida")');
    return;
  }
  var pantallaAncho:Int = untyped __cpp__("(int)__gfx.width");
  var pantallaAlto:Int = untyped __cpp__("(int)__gfx.height");
  untyped __cpp__('sxn_log_num("gfx: pantalla ancho", {0})', pantallaAncho);
  untyped __cpp__('sxn_log_num("gfx: pantalla alto", {0})', pantallaAlto);

  var acquire:Int = untyped __cpp__("(int)sxn_gfx_acquire()");
  if (acquire != 0) {
    untyped __cpp__('sxn_log("gfx: display ocupado (EBUSY), demo omitida")');
    return;
  }

  // Un cuadrante de la pantalla alcanza para la demo (y entra comodo en la
  // arena de 4 MiB del heap nativo).
  var lienzo = new Lienzo(640, 400);
  lienzo.degrade();
  lienzo.rectangulo(220, 140, 200, 120, 0x00CC44);

  var present = lienzo.presentar();
  untyped __cpp__('sxn_log_num("gfx: present", {0})', present);

  var release:Int = untyped __cpp__("(int)sxn_gfx_release()");
  untyped __cpp__('sxn_log_num("gfx: release", {0})', release);

  if (present != 0 || release != 0) {
    untyped __cpp__('sxn_log("gfx: fallo la demo grafica")');
    untyped __cpp__("sxn_exit(1)");
  }
  untyped __cpp__('sxn_log("gfx: demo grafica OK")');
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

  // Null<T> (std::optional): buscar valores, sumar los presentes y contar los
  // nulos. Claves 1 y 2 devuelven 100 y 200; 0 y 3 devuelven null.
  var sumaOpt = 0;
  var nulos = 0;
  for (clave in 0...4) {
    var valor:Null<Int> = buscarValor(clave);
    if (valor != null) {
      var v:Int = valor; // null-safety promueve Null<Int> -> Int
      sumaOpt += v;
    } else {
      nulos += 1;
    }
  }
  untyped __cpp__('sxn_log_num("main: suma Null<Int> (esperado 300)", {0})', sumaOpt);
  untyped __cpp__('sxn_log_num("main: nulos (esperado 2)", {0})', nulos);
  if (sumaOpt != 300 || nulos != 2) {
    untyped __cpp__('sxn_log("main: Null<T> incorrecto")');
    untyped __cpp__("sxn_exit(1)");
  }
  untyped __cpp__('sxn_log("main: Null<T> de Haxe OK")');

  // Map<String,Int> real de Haxe (std::map sobre el mini <map> del SDK). set,
  // exists, get (Null<T>), remove e iteracion por claves.
  var mapa = new Map<String, Int>();
  mapa.set("uno", 1);
  mapa.set("dos", 2);
  mapa.set("tres", 3);
  var existeDos = mapa.exists("dos") ? 1 : 0;
  mapa.remove("uno");
  var existeUno = mapa.exists("uno") ? 1 : 0;
  var sumaMapa = 0;
  var claves = 0;
  for (k in mapa.keys()) {
    var mv:Null<Int> = mapa.get(k);
    if (mv != null) {
      var m2:Int = mv;
      sumaMapa += m2;
    }
    claves += 1;
  }
  untyped __cpp__('sxn_log_num("main: map existe dos (esperado 1)", {0})', existeDos);
  untyped __cpp__('sxn_log_num("main: map existe uno tras remove (esperado 0)", {0})', existeUno);
  untyped __cpp__('sxn_log_num("main: map suma tras remove (esperado 5)", {0})', sumaMapa);
  untyped __cpp__('sxn_log_num("main: map claves (esperado 2)", {0})', claves);
  if (existeDos != 1 || existeUno != 0 || sumaMapa != 5 || claves != 2) {
    untyped __cpp__('sxn_log("main: Map incorrecto")');
    untyped __cpp__("sxn_exit(1)");
  }
  untyped __cpp__('sxn_log("main: Map<String,Int> de Haxe OK")');

  demoGrafica();

  untyped __cpp__('sxn_log("NATIVE HELLO PASS")');
}
