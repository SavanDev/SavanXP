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

		// Lista propia = defaults del framework MENOS RemovePureExpressions,
		// que elimina los `if` cuyos cuerpos son solo inyecciones untyped
		// __cpp__ (los considera puros y descarta la rama: codigo incorrecto
		// que compila). El DCE fino ya lo hace clang sobre el C++ generado.
		// Al final va nuestro UniqueLocalNames (ver ese archivo).
		preprocessors.resize(0);
		preprocessors.push(SanitizeEverythingIsExpression({}));
		preprocessors.push(PreventRepeatVariables({}));
		preprocessors.push(RemoveSingleExpressionBlocks);
		preprocessors.push(RemoveConstantBoolIfs);
		preprocessors.push(RemoveUnnecessaryBlocks);
		preprocessors.push(RemoveReassignedVariableDeclarations);
		preprocessors.push(RemoveLocalVariableAliases);
		preprocessors.push(MarkUnusedVariables);
		preprocessors.push(Custom(new UniqueLocalNames()));
	}
}

#end
