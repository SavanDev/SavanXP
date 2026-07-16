// SavanXP - toolkit sxgui nativo (Haxe): Dialog modal.
//
// Cuadro modal centrado con barra de titulo (navy), marco levantado, unas
// lineas de texto y un boton de cierre. Replica las metricas de sxgui.c:
// borde de 3px, alto de titulo = altoTexto+6, y el tamano se pide en
// coordenadas de CLIENTE (el marco y el titulo se suman aparte), igual que
// sxgui_dialog_begin.
//
// Alcance: alcanza para dialogs tipo "Acerca de" (texto + OK). Si hace falta
// un dialog con widgets arbitrarios, este es el lugar para crecerlo.

class Dialog {
  public var titulo:String;
  public var lineas:Array<String>;
  public var activo:Bool;
  public var ok:Boton;

  var anchoCliente:Int;
  var altoCliente:Int;
  var okRelX:Int;
  var okRelY:Int;

  public static inline var BORDE:Int = 3;

  public function new(titulo:String, anchoCliente:Int, altoCliente:Int, okRelX:Int, okRelY:Int) {
    this.titulo = titulo;
    this.anchoCliente = anchoCliente;
    this.altoCliente = altoCliente;
    this.okRelX = okRelX;
    this.okRelY = okRelY;
    this.lineas = [];
    this.activo = false;
    this.ok = new Boton(0, 0, 100, 26, "OK");
  }

  function altoTitulo(p:Painter):Int {
    return p.altoTexto() + 6;
  }

  function anchoTotal():Int {
    return anchoCliente + BORDE * 2;
  }

  function altoTotal(p:Painter):Int {
    return altoCliente + BORDE * 2 + altoTitulo(p);
  }

  public function ejeX(p:Painter):Int {
    var v = Std.int((p.ancho - anchoTotal()) / 2);
    return v < 0 ? 0 : v;
  }

  public function ejeY(p:Painter):Int {
    var v = Std.int((p.alto - altoTotal(p)) / 2);
    return v < 0 ? 0 : v;
  }

  // El dialog se centra segun el painter, asi que el boton se reposiciona en
  // coords absolutas antes de dibujar o de hacer hit-test.
  public function sincronizar(p:Painter) {
    ok.x = ejeX(p) + BORDE + okRelX;
    ok.y = ejeY(p) + BORDE + altoTitulo(p) + okRelY;
  }

  public function abrir(p:Painter) {
    activo = true;
    ok.pressed = false;
    sincronizar(p);
  }

  public function cerrar() {
    activo = false;
    ok.pressed = false;
  }

  public function dibujar(p:Painter) {
    if (!activo) {
      return;
    }
    sincronizar(p);

    var dx = ejeX(p);
    var dy = ejeY(p);
    var dw = anchoTotal();
    var dh = altoTotal(p);
    var th = altoTitulo(p);

    p.rect(dx, dy, dw, dh, Painter.FACE);
    p.raised(dx, dy, dw, dh);

    // Barra de titulo.
    p.rect(dx + BORDE, dy + BORDE, dw - BORDE * 2, th, Painter.SELECT);
    p.texto(dx + BORDE + 4, dy + BORDE + Std.int((th - p.altoTexto()) / 2), titulo,
            Painter.SELECT_TEXT);

    // Lineas del area cliente.
    var cx = dx + BORDE;
    var cy = dy + BORDE + th;
    var i = 0;
    while (i < lineas.length) {
      p.label(cx + 10, cy + 8 + i * 20, lineas[i]);
      i += 1;
    }

    ok.dibujar(p);
  }
}
