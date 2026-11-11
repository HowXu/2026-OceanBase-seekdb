"""Shared SQL/frontend/core error contract."""


class DatabaseError(Exception):
    def __init__(self, code: str, message: str):
        if not isinstance(code, str) or not code:
            raise ValueError("DatabaseError requires a nonempty string code")
        self.code = code
        self.message = message
        super().__init__(message)
