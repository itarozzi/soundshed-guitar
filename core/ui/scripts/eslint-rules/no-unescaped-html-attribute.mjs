/**
 * soundshed/no-unescaped-html-attribute
 *
 * Flags `${value}` inside a quoted attribute of markup built with a template
 * literal (`<div data-node-id="${node.id}">`) when `value` is typed as a plain
 * string and is not passed through `escapeHtml`.
 *
 * Attribute values are the one HTML position where the rule can be exact: an
 * attribute never legitimately holds markup, so any non-literal string there
 * must be escaped, and escaping an id or a class name that has no special
 * characters changes nothing. Imported presets and Tone Sharing packs supply
 * node ids, names and paths, and one unescaped `"` in any of them ends the
 * attribute and starts a new one (`onerror=`...).
 *
 * Text positions (`<span>${label}</span>`) are not checked: there a string is
 * as likely to be a trusted fragment (an icon, a sub-template) as data, and
 * the type cannot tell them apart.
 *
 * Numbers, booleans, string-literal unions (`"prev" | "next"`), `toFixed()`,
 * `String(<non-string>)` and a `const` initialised from `escapeHtml(...)` pass.
 * The fix wraps the value in `escapeHtml(...)`; the import is left to the author.
 */

import ts from "typescript";

const ESCAPERS = new Set(["escapeHtml"]);

function isUnsafeStringType(type) {
  if (type.isUnion()) {
    return type.types.some(isUnsafeStringType);
  }
  if (type.flags & (ts.TypeFlags.StringLiteral | ts.TypeFlags.TemplateLiteral)) {
    // A template-literal type is built from its parts; `${number}px` is safe,
    // `${string}` is not, and the checker widens the unsafe ones to string.
    return false;
  }
  return Boolean(type.flags & (ts.TypeFlags.String | ts.TypeFlags.Any | ts.TypeFlags.Unknown));
}

function calleeName(callee) {
  if (callee.type === "Identifier") {
    return callee.name;
  }
  return callee.type === "MemberExpression" && callee.property.type === "Identifier" ? callee.property.name : "";
}

/** Number formatting: its output is digits, a sign, a point and an exponent. */
const NUMBER_FORMATTERS = new Set(["toFixed", "toPrecision", "toExponential"]);

/** A TypeScript node that is already escaped: an escaper call, or a string literal. */
function isEscapedTsNode(node) {
  while (ts.isParenthesizedExpression(node)) {
    node = node.expression;
  }
  if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) {
    return true;
  }
  if (ts.isConditionalExpression(node)) {
    return isEscapedTsNode(node.whenTrue) && isEscapedTsNode(node.whenFalse);
  }
  if (ts.isBinaryExpression(node)
    && [ts.SyntaxKind.BarBarToken, ts.SyntaxKind.QuestionQuestionToken].includes(node.operatorToken.kind)) {
    return isEscapedTsNode(node.left) && isEscapedTsNode(node.right);
  }
  if (ts.isCallExpression(node)) {
    const callee = node.expression;
    const name = ts.isIdentifier(callee) ? callee.text : ts.isPropertyAccessExpression(callee) ? callee.name.text : "";
    return ESCAPERS.has(name);
  }
  return false;
}

function isEscaped(expression, services, checker) {
  if (expression.type === "CallExpression") {
    const name = calleeName(expression.callee);
    if (ESCAPERS.has(name) || NUMBER_FORMATTERS.has(name)) {
      return true;
    }
    // String(flag), String(count): only a string argument can carry markup.
    if (name === "String" && expression.callee.type === "Identifier" && expression.arguments.length === 1) {
      const argument = services.esTreeNodeToTSNodeMap.get(expression.arguments[0]);
      return !isUnsafeStringType(checker.getTypeAtLocation(argument));
    }
    return false;
  }
  // `const safeTitle = escapeHtml(item.title)`, used later as `${safeTitle}`.
  if (expression.type === "Identifier") {
    const symbol = checker.getSymbolAtLocation(services.esTreeNodeToTSNodeMap.get(expression));
    const declaration = symbol?.valueDeclaration;
    if (declaration && ts.isVariableDeclaration(declaration) && declaration.initializer
      && (ts.getCombinedNodeFlags(declaration) & ts.NodeFlags.Const)) {
      return isEscapedTsNode(declaration.initializer);
    }
  }
  return false;
}

const ATTRIBUTE_NAME = String.raw`(?:data-[a-z0-9-]+|aria-[a-z-]+|title|class|id|style|href|src|alt|value|name|placeholder|for|type|role|tabindex|draggable|download|target|rel|width|height|label|content|srcset|poster|action|lang|dir)`;

/**
 * A literal that holds only attributes, built to be spliced into a tag elsewhere:
 * `data-edge-from="${edge.from}" data-edge-to="${edge.to}"`, or ` title="${title}"`.
 * Bare words are allowed before it (`disabled`, an earlier interpolation's stand-in), but
 * the attribute being opened must be a real one, so a log line reading `key="${key}"` is not
 * mistaken for markup.
 */
const ATTRIBUTE_FRAGMENT = new RegExp(
  String.raw`^(?:\s*[a-z][a-z0-9-]*(?:\s*=\s*(?:"[^"]*"|'[^']*'))?)*\s*${ATTRIBUTE_NAME}\s*=\s*(?:"[^"]*|'[^']*)$`,
);

/** True when `before` ends inside an HTML start tag, in an open quoted attribute value. */
function endsInQuotedAttribute(before) {
  const tagStart = before.lastIndexOf("<");
  if (tagStart < 0 && before.lastIndexOf(">") < 0) {
    return ATTRIBUTE_FRAGMENT.test(before);
  }
  if (tagStart < 0 || before.lastIndexOf(">") > tagStart || !/^<[a-zA-Z]/.test(before.slice(tagStart))) {
    return false;
  }
  const insideTag = before.slice(tagStart);
  // Walk the tag's attributes so a closed `a="x"` earlier in it does not count.
  let quote = "";
  for (let i = 0; i < insideTag.length; i += 1) {
    const ch = insideTag[i];
    if (quote) {
      if (ch === quote) {
        quote = "";
      }
    } else if ((ch === '"' || ch === "'") && /=\s*$/.test(insideTag.slice(0, i))) {
      quote = ch;
    }
  }
  return quote !== "";
}

export default {
  meta: {
    type: "problem",
    fixable: "code",
    docs: { description: "Require escapeHtml for string values interpolated into HTML attributes" },
    messages: {
      unescaped: "String interpolated into an HTML attribute without escapeHtml(): `{{text}}`.",
    },
    schema: [],
  },
  create(context) {
    const services = context.sourceCode.parserServices;
    if (!services?.program || !services.esTreeNodeToTSNodeMap) {
      return {};
    }
    const checker = services.program.getTypeChecker();

    return {
      TemplateLiteral(node) {
        if (node.parent?.type === "TaggedTemplateExpression" || node.expressions.length === 0) {
          return;
        }
        let before = "";
        node.expressions.forEach((expression, index) => {
          // Stand-in for earlier interpolations: a letter never opens or closes a quote or a tag.
          before += (index > 0 ? "x" : "") + node.quasis[index].value.raw;
          if (!endsInQuotedAttribute(before) || isEscaped(expression, services, checker)) {
            return;
          }
          const tsNode = services.esTreeNodeToTSNodeMap.get(expression);
          if (!isUnsafeStringType(checker.getTypeAtLocation(tsNode))) {
            return;
          }
          const text = context.sourceCode.getText(expression);
          context.report({
            node: expression,
            messageId: "unescaped",
            data: { text: text.length > 60 ? `${text.slice(0, 57)}...` : text },
            fix: (fixer) => fixer.replaceText(expression, `escapeHtml(${text})`),
          });
        });
      },
    };
  },
};
