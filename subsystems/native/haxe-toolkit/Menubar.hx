// SavanXP - toolkit sxgui nativo (Haxe): Menubar.
//
// Barra de menu con dropdowns, replicando el look y las metricas de sxgui.c:
// altura = altoTexto+8, filas de altoTexto+4, separadores de 6px, gutter de 20,
// titulo abierto en navy, popup con bisel levantado y separadores etched.
//
// El despacho de comandos es por ID (como las tablas de menu de sxgui en C):
// la app pregunta por el id del item clickeado y decide -- asi se evita
// depender de closures, que el codegen de reflaxe.CPP prefiere no usar.

class Menubar {
  public var menus:Array<Menu>;
  public var abierto:Int;   // menu desplegado, -1 = ninguno
  public var resaltado:Int; // item bajo el cursor en el desplegado, -1

  public static inline var TITLE_PAD:Int = 8;
  public static inline var SEPARATOR_H:Int = 6;
  public static inline var GUTTER:Int = 20;

  public function new() {
    this.menus = [];
    this.abierto = -1;
    this.resaltado = -1;
  }

  public function agregar(menu:Menu):Menubar {
    menus.push(menu);
    return this;
  }

  public function altura(p:Painter):Int {
    return p.altoTexto() + 8;
  }

  function alturaFila(p:Painter):Int {
    return p.altoTexto() + 4;
  }

  function alturaItem(p:Painter, it:MenuItem):Int {
    return it.esSeparador() ? SEPARATOR_H : alturaFila(p);
  }

  function tituloX(p:Painter, index:Int):Int {
    var x = 2;
    var i = 0;
    while (i < index) {
      x += p.anchoTexto(menus[i].titulo) + TITLE_PAD * 2;
      i += 1;
    }
    return x;
  }

  function tituloW(p:Painter, index:Int):Int {
    return p.anchoTexto(menus[index].titulo) + TITLE_PAD * 2;
  }

  // Indice del titulo bajo (px,py), o -1.
  public function tituloEn(p:Painter, px:Int, py:Int):Int {
    if (py < 0 || py >= altura(p)) {
      return -1;
    }
    var i = 0;
    while (i < menus.length) {
      var x = tituloX(p, i);
      if (px >= x && px < x + tituloW(p, i)) {
        return i;
      }
      i += 1;
    }
    return -1;
  }

  function popupW(p:Painter, index:Int):Int {
    var w = 0;
    var menu = menus[index];
    var i = 0;
    while (i < menu.items.length) {
      if (!menu.items[i].esSeparador()) {
        var tw = p.anchoTexto(menu.items[i].text);
        if (tw > w) {
          w = tw;
        }
      }
      i += 1;
    }
    return w + GUTTER + 12;
  }

  function popupH(p:Painter, index:Int):Int {
    var h = 2;
    var menu = menus[index];
    var i = 0;
    while (i < menu.items.length) {
      h += alturaItem(p, menu.items[i]);
      i += 1;
    }
    return h;
  }

  // x del popup, recortado al ancho de la superficie.
  function popupX(p:Painter, index:Int):Int {
    var x = tituloX(p, index);
    var w = popupW(p, index);
    if (x + w > p.ancho) {
      x = p.ancho - w;
    }
    if (x < 0) {
      x = 0;
    }
    return x;
  }

  public function enPopup(p:Painter, px:Int, py:Int):Bool {
    if (abierto < 0) {
      return false;
    }
    var x = popupX(p, abierto);
    var y = altura(p);
    return px >= x && px < x + popupW(p, abierto) && py >= y && py < y + popupH(p, abierto);
  }

  // Indice del item bajo (px,py) en el popup abierto; -1 sobre separadores o
  // fuera del popup.
  public function itemEn(p:Painter, px:Int, py:Int):Int {
    if (!enPopup(p, px, py)) {
      return -1;
    }
    var menu = menus[abierto];
    var rowY = altura(p) + 1;
    var i = 0;
    while (i < menu.items.length) {
      var ih = alturaItem(p, menu.items[i]);
      if (py >= rowY && py < rowY + ih) {
        return menu.items[i].esSeparador() ? -1 : i;
      }
      rowY += ih;
      i += 1;
    }
    return -1;
  }

  // Id del item bajo (px,py), o -1 (para el despacho de comandos).
  public function idEn(p:Painter, px:Int, py:Int):Int {
    var idx = itemEn(p, px, py);
    return idx >= 0 ? menus[abierto].items[idx].id : -1;
  }

  public function cerrar() {
    abierto = -1;
    resaltado = -1;
  }

  public function dibujar(p:Painter) {
    var h = altura(p);
    p.rect(0, 0, p.ancho, h, Painter.FACE);
    p.hline(0, h - 1, p.ancho, Painter.SHADOW);

    var ty = Std.int((h - p.altoTexto()) / 2);
    var i = 0;
    while (i < menus.length) {
      var x = tituloX(p, i);
      var w = tituloW(p, i);
      var color = Painter.TEXT;
      if (i == abierto) {
        p.rect(x, 0, w, h, Painter.SELECT);
        color = Painter.SELECT_TEXT;
      }
      p.texto(x + TITLE_PAD, ty, menus[i].titulo, color);
      i += 1;
    }

    if (abierto >= 0) {
      dibujarPopup(p);
    }
  }

  function dibujarPopup(p:Painter) {
    var menu = menus[abierto];
    var x = popupX(p, abierto);
    var y = altura(p);
    var w = popupW(p, abierto);
    var h = popupH(p, abierto);

    p.rect(x, y, w, h, Painter.FACE);
    p.raised(x, y, w, h);

    var rowY = y + 1;
    var i = 0;
    while (i < menu.items.length) {
      var it = menu.items[i];
      var ih = alturaItem(p, it);
      if (it.esSeparador()) {
        var ly = rowY + Std.int(ih / 2) - 1;
        p.hline(x + 2, ly, w - 4, Painter.SHADOW);
        p.hline(x + 2, ly + 1, w - 4, Painter.LIGHT);
      } else {
        var color = Painter.TEXT;
        if (i == resaltado) {
          p.rect(x + 1, rowY, w - 2, ih, Painter.SELECT);
          color = Painter.SELECT_TEXT;
        }
        p.texto(x + GUTTER, rowY + 2, it.text, color);
      }
      rowY += ih;
      i += 1;
    }
  }
}
