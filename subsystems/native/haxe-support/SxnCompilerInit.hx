// SavanXP - init del compilador reflaxe.CPP para el subsistema nativo.
//
// Envuelve cxxcompiler.CompilerInit.Start() y agrega el preprocesador
// UniqueLocalNames al final de la lista: hace unicos los nombres de locals por
// funcion, porque el codegen de reflaxe.CPP aplana bloques hermanos al emitir
// y dos scopes de Haxe con el mismo nombre (los `_g` de los for-in) terminan
// como declaraciones repetidas en el mismo scope de C++.
//
// Se invoca desde el hxml del build con:
//   --macro SxnCompilerInit.Start()
// en lugar de cxxcompiler.CompilerInit.Start().
#if macro

import reflaxe.ReflectCompiler;
import reflaxe.preprocessors.ExpressionPreprocessor;

class SxnCompilerInit {
	public static function Start() {
		cxxcompiler.CompilerInit.Start();

		final compilers = ReflectCompiler.Compilers;
		if (compilers.length == 0) {
			Sys.println("SxnCompilerInit: no hay compiladores registrados");
			return;
		}

		final compiler = compilers[compilers.length - 1];
		final preprocessors = @:privateAccess compiler.expressionPreprocessors;
		preprocessors.push(Custom(new UniqueLocalNames()));
	}
}

#end
