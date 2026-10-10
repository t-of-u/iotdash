"""
Exceptions that the entry point turns into error responses:

    {"statusCode": <status_code>,
     "body": {"success": false, "message": ..., "error_code": ..., "errors": ...}}
"""


class APIException(Exception):
    """Base API error. ``errors`` carries per-field details (dict) when there are any."""

    def __init__(self, message: str, status_code: int = 500, error_code: str = None, errors=None):
        self.message = message
        self.status_code = status_code
        self.error_code = error_code
        self.errors = errors
        super().__init__(message)


class BadRequest(APIException):
    """400"""

    def __init__(self, message: str = "Bad request", error_code: str = "BAD_REQUEST", errors=None):
        super().__init__(message, 400, error_code, errors)


class Unauthorized(APIException):
    """401"""

    def __init__(self, message: str = "Unauthorized", error_code: str = "UNAUTHORIZED", errors=None):
        super().__init__(message, 401, error_code, errors)


class Forbidden(APIException):
    """403"""

    def __init__(self, message: str = "Forbidden", error_code: str = "FORBIDDEN", errors=None):
        super().__init__(message, 403, error_code, errors)


class NotFound(APIException):
    """404"""

    def __init__(self, message: str = "Not found", error_code: str = "NOT_FOUND", errors=None):
        super().__init__(message, 404, error_code, errors)


class Conflict(APIException):
    """409"""

    def __init__(self, message: str = "Conflict", error_code: str = "CONFLICT", errors=None):
        super().__init__(message, 409, error_code, errors)


class Validation(APIException):
    """422"""

    def __init__(self, message: str = "Validation failed", error_code: str = "VALIDATION_ERROR", errors=None):
        super().__init__(message, 422, error_code, errors)


class InternalServer(APIException):
    """500"""

    def __init__(self, message: str = "Internal server error", error_code: str = "INTERNAL_ERROR", errors=None):
        super().__init__(message, 500, error_code, errors)
