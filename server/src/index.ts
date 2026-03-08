// Couch Conduit — Signaling Server (Cloudflare Worker)
//
// Maps short room codes to host endpoints so friends can connect
// without sharing raw IP addresses.
//
// API:
//   POST   /api/rooms          — Host creates a room → { code }
//   GET    /api/rooms/:code    — Client resolves code → { host_ip, host_port }
//   DELETE /api/rooms/:code    — Host deletes room on shutdown
//   GET    /health             — Health check

export interface Env {
	ROOMS: KVNamespace;
}

// Charset excluding confusing characters (0/O, 1/I/L)
const CHARSET = "ABCDEFGHJKMNPQRSTVWXYZ23456789";
const CODE_LENGTH = 6;
const ROOM_TTL_SECONDS = 7200; // 2 hours

function generateCode(): string {
	const arr = new Uint8Array(CODE_LENGTH);
	crypto.getRandomValues(arr);
	return Array.from(arr, (b) => CHARSET[b % CHARSET.length]).join("");
}

const corsHeaders: Record<string, string> = {
	"Access-Control-Allow-Origin": "*",
	"Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
	"Access-Control-Allow-Headers": "Content-Type",
};

function json(data: unknown, status = 200): Response {
	return new Response(JSON.stringify(data), {
		status,
		headers: { "Content-Type": "application/json", ...corsHeaders },
	});
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url);
		const path = url.pathname;

		// CORS preflight
		if (request.method === "OPTIONS") {
			return new Response(null, { status: 204, headers: corsHeaders });
		}

		// ── POST /api/rooms ─ Host creates a room ──────────────────────
		if (request.method === "POST" && path === "/api/rooms") {
			let body: { host_ip?: string; host_port?: number };
			try {
				body = (await request.json()) as {
					host_ip?: string;
					host_port?: number;
				};
			} catch {
				return json({ error: "Invalid JSON body" }, 400);
			}

			if (!body.host_ip || !body.host_port) {
				return json({ error: "Missing host_ip or host_port" }, 400);
			}

			// Generate a unique code (retry up to 10 times on collision)
			let code = "";
			for (let i = 0; i < 10; i++) {
				code = generateCode();
				const existing = await env.ROOMS.get(code);
				if (!existing) break;
			}

			await env.ROOMS.put(
				code,
				JSON.stringify({
					host_ip: body.host_ip,
					host_port: body.host_port,
					created_at: Date.now(),
				}),
				{ expirationTtl: ROOM_TTL_SECONDS }
			);

			return json({ code });
		}

		// ── GET /api/rooms/:code ─ Client resolves a room ──────────────
		const roomMatch = path.match(/^\/api\/rooms\/([A-Z0-9]{4,8})$/i);
		if (roomMatch) {
			const code = roomMatch[1].toUpperCase();

			if (request.method === "GET") {
				const data = await env.ROOMS.get(code);
				if (!data) {
					return json({ error: "Room not found" }, 404);
				}
				const room = JSON.parse(data) as {
					host_ip: string;
					host_port: number;
				};
				return json({
					host_ip: room.host_ip,
					host_port: room.host_port,
				});
			}

			// ── DELETE /api/rooms/:code ─ Host removes room ────────────
			if (request.method === "DELETE") {
				await env.ROOMS.delete(code);
				return json({ ok: true });
			}
		}

		// ── GET /health ────────────────────────────────────────────────
		if (path === "/health") {
			return json({ status: "ok", service: "couch-conduit-signaling" });
		}

		return json({ error: "Not found" }, 404);
	},
};
