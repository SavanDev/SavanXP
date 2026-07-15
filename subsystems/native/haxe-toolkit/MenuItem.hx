// SavanXP - toolkit sxgui nativo (Haxe): item de menu.
//
// Un item con texto y un id de comando. Texto vacio = separador (como el
// {0,0,0} de las tablas de menu de sxgui en C).

class MenuItem {
  public var text:String;
  public var id:Int;

  public function new(text:String, id:Int) {
    this.text = text;
    this.id = id;
  }

  public inline function esSeparador():Bool {
    return text == "";
  }
}

// Nota: no hay factory estatica `separador()` a proposito. Un metodo estatico
// que devuelve el propio tipo de clase genera `static std::shared_ptr<MenuItem>`
// en el header, pero reflaxe.CPP NO agrega el #include <memory> en ese caso (si
// lo hace cuando shared_ptr aparece en un campo) -> no compila. Los separadores
// se construyen con `new MenuItem("", 0)` desde Menu.agregarSeparador().
