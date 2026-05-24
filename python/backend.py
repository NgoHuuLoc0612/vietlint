"""
VietLint FastAPI backend
Provides REST API, WebSocket streaming, and project-wide convention history
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import sqlite3
import time
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, AsyncGenerator, Optional

import uvicorn
from fastapi import (
    FastAPI, HTTPException, WebSocket, WebSocketDisconnect,
    BackgroundTasks, Depends, Query, Request, status,
)
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel, Field, validator

# Import C++ extension - graceful fallback for CI environments without compiled module
try:
    import vietlint_core as _core
    _ENGINE_AVAILABLE = True
except ImportError:
    _ENGINE_AVAILABLE = False
    logging.warning("vietlint_core not available - running in stub mode")

logger = logging.getLogger("vietlint.backend")

# ---------------------------------------------------------------------------
# Database layer
# ---------------------------------------------------------------------------
DB_PATH = Path(os.environ.get("VIETLINT_DB", "vietlint_history.db"))

SCHEMA_SQL = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS projects (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    root_path   TEXT NOT NULL,
    created_at  TEXT NOT NULL,
    config_json TEXT
);

CREATE TABLE IF NOT EXISTS scan_runs (
    id           TEXT PRIMARY KEY,
    project_id   TEXT REFERENCES projects(id) ON DELETE CASCADE,
    started_at   TEXT NOT NULL,
    finished_at  TEXT,
    files_count  INTEGER DEFAULT 0,
    error_count  INTEGER DEFAULT 0,
    warning_count INTEGER DEFAULT 0,
    info_count   INTEGER DEFAULT 0,
    triggered_by TEXT DEFAULT 'api'
);

CREATE TABLE IF NOT EXISTS violations (
    id           TEXT PRIMARY KEY,
    run_id       TEXT REFERENCES scan_runs(id) ON DELETE CASCADE,
    project_id   TEXT REFERENCES projects(id) ON DELETE CASCADE,
    file_path    TEXT NOT NULL,
    rule_id      TEXT NOT NULL,
    severity     TEXT NOT NULL CHECK(severity IN ('error','warning','info','fatal')),
    message      TEXT NOT NULL,
    identifier   TEXT,
    line         INTEGER,
    col          INTEGER,
    start_byte   INTEGER,
    end_byte     INTEGER,
    fixes_json   TEXT,
    created_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS rule_stats (
    project_id  TEXT REFERENCES projects(id) ON DELETE CASCADE,
    rule_id     TEXT NOT NULL,
    total_count INTEGER DEFAULT 0,
    last_seen   TEXT,
    PRIMARY KEY (project_id, rule_id)
);

CREATE INDEX IF NOT EXISTS idx_violations_project ON violations(project_id);
CREATE INDEX IF NOT EXISTS idx_violations_run     ON violations(run_id);
CREATE INDEX IF NOT EXISTS idx_violations_rule    ON violations(rule_id);
CREATE INDEX IF NOT EXISTS idx_violations_file    ON violations(file_path);
CREATE INDEX IF NOT EXISTS idx_scan_runs_project  ON scan_runs(project_id);
"""

class Database:
    """Thread-safe SQLite wrapper using asyncio."""

    def __init__(self, path: Path) -> None:
        self._path = path
        self._conn: Optional[sqlite3.Connection] = None

    def connect(self) -> None:
        self._conn = sqlite3.connect(str(self._path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.executescript(SCHEMA_SQL)
        self._conn.commit()

    def close(self) -> None:
        if self._conn:
            self._conn.close()

    def execute(self, sql: str, params: tuple = ()) -> sqlite3.Cursor:
        assert self._conn, "Database not connected"
        return self._conn.execute(sql, params)

    def executemany(self, sql: str, params_list: list) -> None:
        assert self._conn
        self._conn.executemany(sql, params_list)
        self._conn.commit()

    def commit(self) -> None:
        assert self._conn
        self._conn.commit()

    def fetchall(self, sql: str, params: tuple = ()) -> list[dict]:
        cur = self.execute(sql, params)
        return [dict(row) for row in cur.fetchall()]

    def fetchone(self, sql: str, params: tuple = ()) -> Optional[dict]:
        cur = self.execute(sql, params)
        row = cur.fetchone()
        return dict(row) if row else None


db = Database(DB_PATH)

# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------
class LintRequest(BaseModel):
    source:   str        = Field(..., description="Source code to lint")
    filename: str        = Field("input.py", description="Filename for language detection")
    format:   str        = Field("json", description="Output format: json|text|sarif|lsp|gcc")
    project_id: Optional[str] = None

class LintFileRequest(BaseModel):
    path:       str
    project_id: Optional[str] = None

class ProjectCreateRequest(BaseModel):
    name:       str
    root_path:  str
    config:     Optional[dict] = None

class Diagnostic(BaseModel):
    file:       str
    rule_id:    str
    severity:   str
    message:    str
    identifier: str = ""
    line:       int = 0
    col:        int = 0
    fixes:      list[str] = []

class LintResponse(BaseModel):
    request_id:  str
    filename:    str
    diagnostics: list[Diagnostic]
    stats: dict[str, int]
    duration_ms: float

class ProjectResponse(BaseModel):
    id:         str
    name:       str
    root_path:  str
    created_at: str

class ScanRunResponse(BaseModel):
    id:            str
    project_id:    str
    started_at:    str
    finished_at:   Optional[str]
    files_count:   int
    error_count:   int
    warning_count: int
    info_count:    int

# ---------------------------------------------------------------------------
# Application lifespan
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    db.connect()
    logger.info("VietLint backend started. DB: %s", DB_PATH)
    yield
    db.close()
    logger.info("VietLint backend stopped")

# ---------------------------------------------------------------------------
# App factory
# ---------------------------------------------------------------------------
def create_app() -> FastAPI:
    app = FastAPI(
        title="VietLint API",
        description="Vietnamese coding convention linter — REST API",
        version="1.0.0",
        lifespan=lifespan,
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.add_middleware(GZipMiddleware, minimum_size=1000)

    return app

app = create_app()

# ---------------------------------------------------------------------------
# Dependency: engine singleton
# ---------------------------------------------------------------------------
_engine_instance = None

def get_engine():
    global _engine_instance
    if _engine_instance is None and _ENGINE_AVAILABLE:
        _engine_instance = _core.LintEngine({})
    return _engine_instance

# ---------------------------------------------------------------------------
# Helper: run lint and store results
# ---------------------------------------------------------------------------
def _run_lint(source: str, filename: str, project_id: Optional[str] = None) -> dict:
    start = time.perf_counter()
    engine = get_engine()

    if engine is None:
        # Stub mode
        return {
            "diagnostics": [],
            "stats": {"files_scanned": 0, "errors": 0, "warnings": 0, "infos": 0},
            "duration_ms": 0.0,
        }

    raw_diags = engine.lint_source(source, filename)
    duration_ms = (time.perf_counter() - start) * 1000.0

    diagnostics = [Diagnostic(**d).dict() for d in raw_diags]
    stats = engine.stats()

    if project_id:
        _store_violations(diagnostics, project_id, filename)

    return {
        "diagnostics": diagnostics,
        "stats": dict(stats),
        "duration_ms": round(duration_ms, 2),
    }

def _store_violations(diagnostics: list[dict], project_id: str, filename: str) -> None:
    now = datetime.now(timezone.utc).isoformat()
    rows = [
        (
            str(uuid.uuid4()),
            project_id,
            filename,
            d.get("rule_id", ""),
            d.get("severity", "info"),
            d.get("message", ""),
            d.get("identifier", ""),
            d.get("line", 0),
            d.get("col", 0),
            d.get("start_byte", 0),
            d.get("end_byte", 0),
            json.dumps(d.get("fixes", [])),
            now,
        )
        for d in diagnostics
    ]
    if rows:
        db.executemany(
            """INSERT OR IGNORE INTO violations
               (id,project_id,file_path,rule_id,severity,message,identifier,
                line,col,start_byte,end_byte,fixes_json,created_at)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            rows,
        )

# ---------------------------------------------------------------------------
# Routes: lint
# ---------------------------------------------------------------------------
@app.post("/api/v1/lint", response_model=LintResponse, tags=["lint"])
async def lint_source(req: LintRequest) -> LintResponse:
    """Lint source code text. Returns diagnostics with optional project history storage."""
    result = await asyncio.get_event_loop().run_in_executor(
        None, _run_lint, req.source, req.filename, req.project_id
    )
    return LintResponse(
        request_id   = str(uuid.uuid4()),
        filename     = req.filename,
        diagnostics  = [Diagnostic(**d) for d in result["diagnostics"]],
        stats        = result["stats"],
        duration_ms  = result["duration_ms"],
    )

@app.post("/api/v1/lint/file", response_model=LintResponse, tags=["lint"])
async def lint_file(req: LintFileRequest) -> LintResponse:
    """Lint a file on disk. Returns diagnostics."""
    path = Path(req.path)
    if not path.exists():
        raise HTTPException(status_code=404, detail=f"File not found: {req.path}")
    source = path.read_text(encoding="utf-8", errors="replace")
    result = await asyncio.get_event_loop().run_in_executor(
        None, _run_lint, source, req.path, req.project_id
    )
    return LintResponse(
        request_id  = str(uuid.uuid4()),
        filename    = req.path,
        diagnostics = [Diagnostic(**d) for d in result["diagnostics"]],
        stats       = result["stats"],
        duration_ms = result["duration_ms"],
    )

@app.post("/api/v1/lint/format", tags=["lint"])
async def lint_and_format(req: LintRequest) -> JSONResponse:
    """Lint and return diagnostics in specified output format (text/sarif/gcc/lsp)."""
    result = await asyncio.get_event_loop().run_in_executor(
        None, _run_lint, req.source, req.filename, None
    )
    engine = get_engine()
    if engine is None:
        return JSONResponse(content={"output": ""})

    formatted = engine.format_diagnostics(
        result["diagnostics"], req.format, req.source
    )
    media = "text/plain" if req.format in ("text", "gcc") else "application/json"
    return JSONResponse(content={"output": formatted, "format": req.format})

# ---------------------------------------------------------------------------
# Routes: streaming via Server-Sent Events
# ---------------------------------------------------------------------------
@app.get("/api/v1/lint/stream", tags=["lint"])
async def lint_stream(
    source:   str = Query(...),
    filename: str = Query("input.py"),
) -> StreamingResponse:
    """Lint source and stream diagnostics as Server-Sent Events."""
    async def generate() -> AsyncGenerator[str, None]:
        engine = get_engine()
        if engine is None:
            yield "data: {\"done\": true}\n\n"
            return

        diags = await asyncio.get_event_loop().run_in_executor(
            None, engine.lint_source, source, filename
        )
        for d in diags:
            yield f"data: {json.dumps(d)}\n\n"
            await asyncio.sleep(0)  # yield control

        stats = dict(engine.stats())
        yield f"data: {{\"done\": true, \"stats\": {json.dumps(stats)}}}\n\n"

    return StreamingResponse(
        generate(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )

# ---------------------------------------------------------------------------
# Routes: WebSocket diagnostic streaming
# ---------------------------------------------------------------------------
class ConnectionManager:
    def __init__(self) -> None:
        self.active: list[WebSocket] = []
        self._lock = asyncio.Lock()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self.active.append(ws)

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self.active = [c for c in self.active if c is not ws]

    async def broadcast(self, message: dict) -> None:
        data = json.dumps(message)
        disconnected = []
        for ws in list(self.active):
            try:
                await ws.send_text(data)
            except WebSocketDisconnect:
                disconnected.append(ws)
        for ws in disconnected:
            await self.disconnect(ws)

ws_manager = ConnectionManager()

@app.websocket("/ws/lint")
async def websocket_lint(websocket: WebSocket) -> None:
    """
    WebSocket endpoint for real-time lint as-you-type.
    Client sends: {"source": "...", "filename": "...", "request_id": "..."}
    Server sends: diagnostic objects then {"done": true, "request_id": "..."}
    """
    await ws_manager.connect(websocket)
    try:
        while True:
            raw = await websocket.receive_text()
            try:
                payload = json.loads(raw)
            except json.JSONDecodeError:
                await websocket.send_json({"error": "Invalid JSON"})
                continue

            source    = payload.get("source", "")
            filename  = payload.get("filename", "input.py")
            req_id    = payload.get("request_id", str(uuid.uuid4()))

            engine = get_engine()
            if engine is None:
                await websocket.send_json({"done": True, "request_id": req_id,
                                           "diagnostics": []})
                continue

            diags = await asyncio.get_event_loop().run_in_executor(
                None, engine.lint_source, source, filename
            )

            # Stream each diagnostic
            for d in diags:
                await websocket.send_json({**d, "request_id": req_id})

            stats = dict(engine.stats())
            await websocket.send_json({
                "done": True,
                "request_id": req_id,
                "stats": stats,
                "count": len(diags),
            })

    except WebSocketDisconnect:
        await ws_manager.disconnect(websocket)

# ---------------------------------------------------------------------------
# Routes: projects
# ---------------------------------------------------------------------------
@app.post("/api/v1/projects", tags=["projects"])
async def create_project(req: ProjectCreateRequest) -> ProjectResponse:
    project_id = str(uuid.uuid4())
    now = datetime.now(timezone.utc).isoformat()
    db.execute(
        "INSERT INTO projects (id, name, root_path, created_at, config_json) VALUES (?,?,?,?,?)",
        (project_id, req.name, req.root_path, now, json.dumps(req.config or {})),
    )
    db.commit()
    return ProjectResponse(
        id=project_id, name=req.name, root_path=req.root_path, created_at=now
    )

@app.get("/api/v1/projects", tags=["projects"])
async def list_projects() -> list[ProjectResponse]:
    rows = db.fetchall("SELECT id, name, root_path, created_at FROM projects ORDER BY created_at DESC")
    return [ProjectResponse(**r) for r in rows]

@app.get("/api/v1/projects/{project_id}", tags=["projects"])
async def get_project(project_id: str) -> ProjectResponse:
    row = db.fetchone("SELECT id, name, root_path, created_at FROM projects WHERE id=?",
                      (project_id,))
    if not row:
        raise HTTPException(status_code=404, detail="Project not found")
    return ProjectResponse(**row)

@app.delete("/api/v1/projects/{project_id}", tags=["projects"])
async def delete_project(project_id: str) -> dict:
    db.execute("DELETE FROM projects WHERE id=?", (project_id,))
    db.commit()
    return {"deleted": True}

# ---------------------------------------------------------------------------
# Routes: violations history
# ---------------------------------------------------------------------------
@app.get("/api/v1/projects/{project_id}/violations", tags=["history"])
async def get_violations(
    project_id: str,
    rule_id:    Optional[str] = Query(None),
    severity:   Optional[str] = Query(None),
    file_path:  Optional[str] = Query(None),
    limit:      int = Query(100, le=1000),
    offset:     int = Query(0),
) -> dict:
    where = ["project_id=?"]
    params: list[Any] = [project_id]
    if rule_id:   where.append("rule_id=?");   params.append(rule_id)
    if severity:  where.append("severity=?");  params.append(severity)
    if file_path: where.append("file_path LIKE ?"); params.append(f"%{file_path}%")

    sql = f"""
        SELECT id, file_path, rule_id, severity, message, identifier,
               line, col, fixes_json, created_at
        FROM violations
        WHERE {' AND '.join(where)}
        ORDER BY created_at DESC
        LIMIT ? OFFSET ?
    """
    params.extend([limit, offset])
    rows = db.fetchall(sql, tuple(params))

    count_sql = f"SELECT COUNT(*) as cnt FROM violations WHERE {' AND '.join(where[:-1] if rows else where)}"
    total_row = db.fetchone(f"SELECT COUNT(*) as cnt FROM violations WHERE {' AND '.join(where)}",
                             tuple(params[:-2]))
    total = total_row["cnt"] if total_row else 0

    for row in rows:
        try:
            row["fixes"] = json.loads(row.get("fixes_json") or "[]")
        except Exception:
            row["fixes"] = []

    return {"total": total, "offset": offset, "limit": limit, "violations": rows}

@app.get("/api/v1/projects/{project_id}/stats", tags=["history"])
async def get_project_stats(project_id: str) -> dict:
    """Aggregated convention statistics for a project."""
    total = db.fetchone(
        "SELECT COUNT(*) as cnt FROM violations WHERE project_id=?", (project_id,)
    )
    by_rule = db.fetchall(
        "SELECT rule_id, COUNT(*) as cnt, severity FROM violations "
        "WHERE project_id=? GROUP BY rule_id ORDER BY cnt DESC",
        (project_id,),
    )
    by_severity = db.fetchall(
        "SELECT severity, COUNT(*) as cnt FROM violations "
        "WHERE project_id=? GROUP BY severity",
        (project_id,),
    )
    by_file = db.fetchall(
        "SELECT file_path, COUNT(*) as cnt FROM violations "
        "WHERE project_id=? GROUP BY file_path ORDER BY cnt DESC LIMIT 20",
        (project_id,),
    )
    return {
        "total_violations": total["cnt"] if total else 0,
        "by_rule":          by_rule,
        "by_severity":      by_severity,
        "top_files":        by_file,
    }

# ---------------------------------------------------------------------------
# Routes: config
# ---------------------------------------------------------------------------
@app.get("/api/v1/rules", tags=["config"])
async def list_rules() -> list[dict]:
    """List all registered lint rules."""
    engine = get_engine()
    if engine is None:
        return []
    return [{"id": rid} for rid in engine.rule_ids()]

@app.get("/api/v1/health", tags=["system"])
async def health_check() -> dict:
    return {
        "status":          "ok",
        "engine_available": _ENGINE_AVAILABLE,
        "db":              str(DB_PATH),
        "timestamp":       datetime.now(timezone.utc).isoformat(),
    }

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> None:
    uvicorn.run(
        "vietlint.backend:app",
        host=os.environ.get("VIETLINT_HOST", "0.0.0.0"),
        port=int(os.environ.get("VIETLINT_PORT", "8765")),
        reload=os.environ.get("VIETLINT_RELOAD", "0") == "1",
        log_level="info",
    )

if __name__ == "__main__":
    main()
