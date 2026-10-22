# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Host-side tooling for talking to a console running tinrom.

Speaks the nops wire protocol, so it also works against a UniROM console and
existing nops builds work against tinrom. The protocol is described in
docs/nops-protocol.md; no NOTPSXSerial source is used here. See NOTICE.md.
"""

__version__ = "0.1.0"
