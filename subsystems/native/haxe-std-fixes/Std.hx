package;

// SavanXP - shadow del Std.hx del _std de reflaxe.CPP (se copia como
// Std.cross.hx encima del de la lib, igual mecanismo que Math.hx).
//
// El Std.hx original usa try/catch (rompe con -fno-exceptions) y std::stof en
// parseFloat (reintroduce Float: std::stof devuelve float y la conversion a
// double necesita __extendsfdf2 soft-float, inexistente en el runtime nativo).
// Este shadow deja el resto de la clase igual y solo cambia parseInt/parseFloat:
//  - parseInt: std::stoi propio del mini <string> (no lanza, entero puro).
//  - parseFloat: stub que devuelve NaN sin tocar float (deuda de Float, ver el
//    README del subsistema nativo).

#if !(core_api || cross || eval)
#error "Please don't add haxe/std to your classpath, instead set HAXE_STD_PATH env var"
#end

@:cxxStd
@:haxeStd
@:includeTypeUtils
@:pseudoCoreApi
@:dontGenerateDynamic
class StdImpl {
	public static function isOfType<_Value, _Type>(v: _Value, t: _Type): Bool {
		untyped __cpp__("if constexpr(!haxe::_unwrap_class<_Type>::iscls) {
	return false;
} else if constexpr(std::is_base_of<typename haxe::_unwrap_class<_Type>::inner, typename haxe::_unwrap_mm<_Value>::inner>::value) {
	return true;
}");
		return false;
	}
}

@:cxxStd
@:haxeStd
@:pseudoCoreApi
@:headerOnly
@:headerInclude("string", true)
class Std {
	@:deprecated('Std.is is deprecated. Use Std.isOfType instead.')
	public extern inline static function is(v: Dynamic, t: Dynamic): Bool return isOfType(v, t);
	public extern inline static function isOfType<_Value, _Type>(v: _Value, t: _Type): Bool {
		return StdImpl.isOfType(v, t);
	}

	@:deprecated('Std.instance() is deprecated. Use Std.downcast() instead.')
	public extern inline static function instance<T:{}, S:T>(value: T, c: Class<S>): S return downcast(value, c);
	public extern inline static function downcast<T: {}, S: T>(value: T, c: Class<S>): Null<S> {
		// TODO: Need to add system for casting based on memory management type before implementing this.
		// I.e: use static_cast/dynamic_cast for pointers, and std::static_pointer_cast/dynamic_pointer_cast for std::shared_ptr
		// Probably not possible to cast value types??? This could get complicated.
		throw "Std.downcast is unimplemented.";
		return null;
	}

	public static function string(s: cxx.DynamicToString): String {
		return s;
	}

	public extern inline static function int(x: Float): Int {
		return untyped __cpp__("((int)({0}))", x);
	}

	public static function parseInt(x: String): Null<Int> {
		// SavanXP: sin -fno-exceptions no hay try/catch; el std::stoi del mini
		// <string> no lanza (entero puro, devuelve 0 ante entrada invalida).
		untyped __cpp__("return std::stoi({0});", x);
		return null;
	}

	public static function parseFloat(x: String): Float {
		// SavanXP: Float aun sin soporte en el runtime nativo (sin soft-float);
		// stub que devuelve NaN como double, sin std::stof ni conversion float.
		untyped __cpp__("(void)({0});", x);
		untyped __cpp__("return __builtin_nan(\"\");");
		return 0.0;
	}

	public extern inline static function random(x: Int): Int {
		if(x <= 1) return 0;
		return Math.floor(Math.random() * x);
	}
}
