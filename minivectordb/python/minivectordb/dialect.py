"""MySQL token-aware entry for the sole nonstandard statement grammar."""
from sqlglot.dialects.mysql import MySQL
from sqlglot.tokens import TokenType

from .errors import DatabaseError

class MiniVectorDialect(MySQL):
    class Parser(MySQL.Parser):
        def _parse_ordered(self, parse_method=None):
            start = self._index
            node = super()._parse_ordered(parse_method)
            # SQLGlot otherwise makes explicit NULLS clauses indistinguishable
            # from its implicit MySQL defaults. Preserve rejection information.
            consumed = self._tokens[start:self._index]
            for left, right in zip(consumed, consumed[1:]):
                if (left.token_type == TokenType.VAR and left.text.upper() == "NULLS"
                        and right.text.upper() in ("FIRST", "LAST")):
                    unsupported("explicit NULLS ordering is unsupported")
            return node


MYSQL = MiniVectorDialect()


def unsupported(message="SQL syntax is outside the supported subset"):
    raise DatabaseError("unsupported_sql", message)


def statement_tokens(sql):
    tokens = MYSQL.tokenize(sql)
    # A single optional trailing terminator, not string/comment contents.
    if tokens and tokens[-1].token_type == TokenType.SEMICOLON:
        tokens = tokens[:-1]
    if not tokens or any(t.token_type == TokenType.SEMICOLON for t in tokens):
        unsupported("expected exactly one SQL statement")
    return tokens


def vector_index(tokens):
    """Recognize all and only CREATE VECTOR INDEX name ON table(column)."""
    prefix = [TokenType.CREATE, TokenType.VECTOR, TokenType.INDEX]
    if [t.token_type for t in tokens[:3]] != prefix:
        return None
    if len(tokens) != 9 or [tokens[i].token_type for i in (4, 6, 8)] != [
            TokenType.ON, TokenType.L_PAREN, TokenType.R_PAREN]:
        unsupported("expected CREATE VECTOR INDEX name ON table(column)")
    names = []
    for i in (3, 5, 7):
        token = tokens[i]
        if token.token_type not in (MYSQL.Parser.ID_VAR_TOKENS | {TokenType.IDENTIFIER, TokenType.VAR}):
            unsupported("expected index, table or column identifier")
        if not token.text or "\0" in token.text:
            unsupported("invalid identifier")
        names.append(token.text)
    return dict(op="create_vector_index", index=names[0], table=names[1], column=names[2])
