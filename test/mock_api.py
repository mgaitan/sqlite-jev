#!/usr/bin/env python3
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def record_for(state, instructions):
    match = re.search(r"rows\[(\d+)\]", instructions)
    if match and isinstance(state, dict) and isinstance(state.get("rows"), list):
        return state["rows"][int(match.group(1))]
    return state


def answer(question, record):
    kind = question["type"]
    text = json.dumps(record, ensure_ascii=False).lower()
    instructions = json.dumps(question.get("instructions", ""), ensure_ascii=False).lower()

    if kind == "noul":
        if "urgent" in instructions or "urgencia" in instructions:
            yes = any(word in text for word in ("asap", "urgente", "hoy", "bloqueado"))
        elif "refund" in instructions or "reembolso" in instructions:
            yes = any(word in text for word in ("refund", "reembolso", "devolución"))
        else:
            yes = "yes" in text or "sí" in text
        return {"type": "noul", "noul": 0.97 if yes else 0.04}

    if kind == "choice":
        criteria = question["criteria"]
        if any(word in text for word in ("cobro", "factura", "reembolso", "tarjeta")):
            chosen = "billing"
        elif any(word in text for word in ("error", "api", "integración", "pantalla")):
            chosen = "technical"
        elif any(word in text for word in ("precio", "plan", "demo")):
            chosen = "sales"
        else:
            chosen = next(iter(criteria))
        probabilities = {
            key: (0.91 if key == chosen else 0.09 / max(1, len(criteria) - 1))
            for key in criteria
        }
        return {
            "type": "choice",
            "choice": chosen,
            "probabilities": probabilities,
            "confidence": 0.88,
        }

    levels = question["criteria"]
    if any(word in text for word in ("furioso", "inaceptable", "estafa")):
        score = float(len(levels) - 1)
    elif any(word in text for word in ("molesto", "frustrado")):
        score = min(1.0, float(len(levels) - 1))
    else:
        score = 0.0
    probabilities = {
        str(i): (1.0 if i == int(score) else 0.0) for i in range(len(levels))
    }
    return {
        "type": "score",
        "score": score,
        "legend": {str(i): value for i, value in enumerate(levels)},
        "probabilities": probabilities,
        "confidence": 0.93,
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(204)
        self.end_headers()

    def do_POST(self):
        try:
            size = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(size))
            answers = {
                key: answer(
                    question,
                    record_for(payload["state"], json.dumps(question.get("instructions", ""))),
                )
                for key, question in payload["questions"].items()
            }
            body = json.dumps(
                {
                    "model": payload["model"],
                    "answers": answers,
                    "usage": {"input_tokens": 101, "output_tokens": 7},
                }
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            body = json.dumps({"error": {"message": str(exc)}}).encode()
            self.send_response(400)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, *_args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
