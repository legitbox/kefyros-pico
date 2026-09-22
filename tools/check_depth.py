with open("/home/legitbox/kefyros-pico/apps/px3_cpu.c") as f:
    lines = f.readlines()

depth = 0
in_exec = False
for i, line in enumerate(lines):
    if "void exec86" in line:
        in_exec = True
        depth = 0
    if in_exec:
        depth += line.count("{") - line.count("}")
        if i >= 3375:
            print(f"{i+1}: depth={depth}: {line.rstrip()}")
