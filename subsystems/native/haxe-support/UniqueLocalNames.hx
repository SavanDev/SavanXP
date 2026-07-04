// SavanXP - preprocesador custom de reflaxe para el subsistema nativo.
//
// Hace UNICOS los nombres de todas las variables locales dentro de cada
// funcion. Motivo: Haxe permite reusar un nombre en scopes hermanos (cada
// for-in declara su contador `_g`), y el codegen de reflaxe.CPP aplana los
// bloques al emitir, con lo que dos `int _g` terminan en el mismo scope de
// C++ ("error: redefinition"). El PreventRepeatVariables del framework solo
// mira la cadena de scopes ANIDADOS, no los hermanos, asi que no alcanza.
//
// Importante: NO se puede mutar TVar.name in-place (cada acceso desde el
// contexto macro materializa un objeto distinto; el rename no llega a los
// TLocal). Igual que PreventRepeatVariablesImpl, reconstruimos el arbol con
// copias del TVar mapeadas por id, renombrando declaracion y usos juntos.
//
// Se registra desde SxnCompilerInit como ExpressionPreprocessor.Custom(...) al
// final de la lista, cuando ya corrieron los pases que mueven bloques.
#if macro

import haxe.macro.Type.TVar;
import haxe.macro.Type.TypedExpr;
import haxe.macro.TypedExprTools;

import reflaxe.BaseCompiler;
import reflaxe.data.ClassFuncData;
import reflaxe.preprocessors.BasePreprocessor;

using reflaxe.helpers.TVarHelper;

class UniqueLocalNames extends BasePreprocessor {
	public function new() {}

	public function process(data: ClassFuncData, compiler: BaseCompiler): Void {
		final original = data.expr;
		if (original == null) {
			return;
		}

		// Semilla: los nombres de los argumentos tambien reservan.
		final used = new Map<String, Int>();
		for (arg in data.args) {
			used.set(arg.getName(), 0);
		}

		final replacements = new Map<Int, TVar>();
		data.setExpr(rename(original, used, replacements));
	}

	function rename(expr: TypedExpr, used: Map<String, Int>, replacements: Map<Int, TVar>): TypedExpr {
		return switch (expr.expr) {
			case TVar(tvar, init):
				final renamedInit = init != null ? rename(init, used, replacements) : null;
				var declared = tvar;
				final base = tvar.name;
				if (used.exists(base)) {
					var suffix = used.get(base) + 1;
					var candidate = base + suffix;
					while (used.exists(candidate)) {
						suffix += 1;
						candidate = base + suffix;
					}
					used.set(base, suffix);
					used.set(candidate, 0);
					declared = tvar.copy(candidate);
					replacements.set(tvar.id, declared);
				} else {
					used.set(base, 0);
				}
				{ expr: TVar(declared, renamedInit), pos: expr.pos, t: expr.t };
			case TLocal(tvar):
				final replacement = replacements.get(tvar.id);
				replacement != null ? { expr: TLocal(replacement), pos: expr.pos, t: expr.t } : expr;
			case _:
				TypedExprTools.map(expr, sub -> rename(sub, used, replacements));
		};
	}
}

#end
