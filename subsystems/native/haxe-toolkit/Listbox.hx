// SavanXP - toolkit sxgui nativo (Haxe): Listbox.
//
// Lista scrollable y seleccionable (campo hundido + items de texto; el item
// seleccionado va en navy con texto blanco, estilo sxgui). Hit-test por fila y
// auto-scroll para mantener visible la seleccion. Compartido por las apps GUI
// nativas; lo usa el port de filesapp.

class Listbox {
  public var x:Int;
  public var y:Int;
  public var w:Int;
  public var h:Int;
  public var items:Array<String>;
  public var seleccion:Int;
  public var scroll:Int;

  public function new(x:Int, y:Int, w:Int, h:Int) {
    this.x = x;
    this.y = y;
    this.w = w;
    this.h = h;
    this.items = [];
    this.seleccion = 0;
    this.scroll = 0;
  }

  public function contiene(px:Int, py:Int):Bool {
    return px >= x && px < x + w && py >= y && py < y + h;
  }

  function alturaFila(p:Painter):Int {
    return p.altoTexto();
  }

  public function filasVisibles(p:Painter):Int {
    var rh = alturaFila(p);
    return rh > 0 ? Std.int((h - 4) / rh) : 0;
  }

  // Indice del item bajo (px,py), o -1 si no hay.
  public function indiceEn(px:Int, py:Int, p:Painter):Int {
    if (!contiene(px, py)) {
      return -1;
    }
    var fila = Std.int((py - (y + 2)) / alturaFila(p));
    var idx = scroll + fila;
    return (idx >= 0 && idx < items.length) ? idx : -1;
  }

  // Ajusta el scroll para que la seleccion quede visible.
  public function asegurarVisible(p:Painter) {
    var vis = filasVisibles(p);
    if (seleccion < scroll) {
      scroll = seleccion;
    } else if (vis > 0 && seleccion >= scroll + vis) {
      scroll = seleccion - vis + 1;
    }
    if (scroll < 0) {
      scroll = 0;
    }
  }

  public function dibujar(p:Painter) {
    p.rect(x, y, w, h, Painter.FIELD);
    p.sunken(x, y, w, h);
    var rh = alturaFila(p);
    var vis = filasVisibles(p);
    var i = 0;
    while (i < vis && scroll + i < items.length) {
      var idx = scroll + i;
      var ry = y + 2 + i * rh;
      if (idx == seleccion) {
        p.rect(x + 2, ry, w - 4, rh, Painter.SELECT);
        p.texto(x + 4, ry, items[idx], Painter.SELECT_TEXT);
      } else {
        p.texto(x + 4, ry, items[idx], Painter.TEXT);
      }
      i += 1;
    }
  }
}
