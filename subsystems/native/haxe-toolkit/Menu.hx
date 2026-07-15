// SavanXP - toolkit sxgui nativo (Haxe): un menu de la barra (titulo + items).

class Menu {
  public var titulo:String;
  public var items:Array<MenuItem>;

  public function new(titulo:String) {
    this.titulo = titulo;
    this.items = [];
  }

  public function agregar(text:String, id:Int):Menu {
    items.push(new MenuItem(text, id));
    return this;
  }

  public function agregarSeparador():Menu {
    items.push(new MenuItem("", 0));
    return this;
  }
}
