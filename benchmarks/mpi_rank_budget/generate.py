from pathlib import Path
p=Path(__file__).parent
p.joinpath("targets.inc").write_text("\n".join(f"TARGET({i:03d}, {i%128})" for i in range(256))+"\n")
p.joinpath("targets.txt").write_text("\n".join(f"pressure_{i:03d}" for i in range(16))+"\n")
