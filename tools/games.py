"""Registro central dos alvos de lifting.

Este é o ÚNICO lugar onde ficam os dados específicos de cada jogo:
endereços de carga, hashes verificados e módulos relevantes.

Regra do projeto: os endereços aqui DEVEM ter sido verificados contra os
binários reais (docs/RECON.md). Endereço não verificado = bug silencioso
em tempo de execução (chamada para o lugar errado).

Uso:
    from games import get_game, GAMES
    game = get_game("x2wr")
    print(game.imagebase("Wolvie.exe"))
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Module:
    """Um PE a ser recompilado estaticamente.

    imagebase/endpoints DEVEM ser não-sobrepostos entre módulos, para que
    o linker/loader possa mapear cada um numa região distinta.
    """

    name: str
    filename: str
    imagebase: int
    size_of_image: int
    sha1: str | None = None
    size: int | None = None
    # Base preference para o scanner de funções
    is_primary: bool = False

    def __post_init__(self) -> None:
        if self.imagebase % 0x1000:
            raise ValueError(f"{self.name}: imagebase não alinhado: {self.imagebase:#x}")
        if self.size_of_image <= 0:
            raise ValueError(f"{self.name}: size_of_image inválido")

    @property
    def end(self) -> int:
        return self.imagebase + self.size_of_image


@dataclass(frozen=True)
class GameConfig:
    """Configuração de um jogo portado."""

    id: str
    display_name: str
    modules: tuple[Module, ...]
    # Diretório/prefixo de onde os assets são lidos em runtime
    asset_dirs: tuple[str, ...] = ()
    # Extensões de arquivo de asset conhecidas (documentação, não parsing)
    asset_exts: tuple[str, ...] = ()
    # Diretório de logs/saves dentro da pasta do jogo
    port_subdir: str = "port"
    # Se o jogo precisa de decodificador de vídeo externo
    video_codec: str | None = None
    # Se o jogo é single-player (rede é stubada)
    single_player: bool = True
    notes: str = ""

    def module(self, name: str) -> Module:
        for m in self.modules:
            if m.name == name or m.filename == name:
                return m
        raise KeyError(f"módulo desconhecido: {name}")

    def imagebase(self, name: str) -> int:
        return self.module(name).imagebase

    def verify(self, game_dir: str) -> list[tuple[str, str, str]]:
        """Confere tamanho/SHA-1 dos binários presentes. Retorna lista de problemas."""
        problems: list[tuple[str, str, str]] = []
        for m in self.modules:
            path = os.path.join(game_dir, m.filename)
            if not os.path.isfile(path):
                problems.append((m.filename, "ausente", "arquivo não encontrado"))
                continue
            actual_size = os.path.getsize(path)
            if m.size is not None and actual_size != m.size:
                problems.append(
                    (m.filename, "tamanho", f"{actual_size} != {m.size}")
                )
            if m.sha1:
                h = hashlib.sha1()
                with open(path, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                if h.hexdigest() != m.sha1:
                    problems.append(
                        (m.filename, "sha1", f"{h.hexdigest()} != {m.sha1}")
                    )
        return problems


# ---------------------------------------------------------------------------
# X2: Wolverine's Revenge (Activision, 2003)
#
# Evidências: docs/RECON.md (Fase 1) — pefile/capstone sobre os binários
# fornecidos em /root/share/X2_Wolverines_Revenge_Win_R_EN/.
#
# ATENÇÃO: estes são os hashes da release *NoCD + widescreen-fix*. Uma retail
# original de CD (2003) tem hashes diferentes e provavelmente SafeDisc.
# ---------------------------------------------------------------------------
X2WR = GameConfig(
    id="x2wr",
    display_name="X2: Wolverine's Revenge",
    modules=(
        Module(
            name="Wolvie",
            filename="Wolvie.exe",
            imagebase=0x00400000,
            size_of_image=0x004CAE8E,
            sha1="37f4327c4ea45d2037e9e177ecbac003469ff490",
            size=2916352,
            is_primary=True,
        ),
        Module(
            name="binkw32",
            filename="binkw32.dll",
            imagebase=0x30000000,
            size_of_image=0x00072000,
            sha1="dd8c012f7e2bb68ec7b47702fcf3ea42e805e1f7",
            size=377856,
        ),
    ),
    asset_dirs=("data", "movies", "music"),
    asset_exts=(".all", ".mdl", ".pso", ".xpr", ".bik"),
    video_codec="bink",
    single_player=True,
    notes=(
        "Direct3D 8 (d3d8.dll) — NÃO D3D9. dxvk-native não serve; usar dx8vk "
        "ou wrapper D3D8->Vulkan próprio. Áudio via DirectSound + Bink(waveOut). "
        "Sem DRM nesta release (NoCD). Containers de asset: PKR3 (data.pkr/wavs.pkr)."
    ),
)


GAMES: dict[str, GameConfig] = {
    X2WR.id: X2WR,
}


def get_game(game_id: str) -> GameConfig:
    try:
        return GAMES[game_id]
    except KeyError:
        known = ", ".join(sorted(GAMES))
        raise SystemExit(
            f"jogo desconhecido: {game_id!r} (conhecidos: {known})"
        ) from None


def list_games() -> list[str]:
    return sorted(GAMES)


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="lista os jogos de lifting suportados")
    ap.add_argument("game", nargs="?")
    ap.add_argument("--game-dir", help="pasta do jogo para verificar hashes")
    args = ap.parse_args()

    if args.game:
        g = get_game(args.game)
        print(f"{g.id}: {g.display_name}")
        for m in g.modules:
            print(
                f"  {m.filename:16s} base={m.imagebase:#010x} "
                f"end={m.end:#010x} size={m.size} sha1={m.sha1}"
            )
        print(f"  notas: {g.notes}")
        if args.game_dir:
            problems = g.verify(args.game_dir)
            if not problems:
                print("  hashes: OK")
            else:
                for name, kind, detail in problems:
                    print(f"  PROBLEMA {name}: {kind}: {detail}")
    else:
        for gid in list_games():
            print(gid)
