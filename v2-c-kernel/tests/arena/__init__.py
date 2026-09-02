# mini-os/v2-c-kernel/tests/arena/
# agent 演练场 · 阶段0（task 契约 + 可复用 gate 判据）
#
# 目标：把"agent 改完内核后如何客观判定回归/达标"的判据，
#       从 baseline_check.py 里抽成无副作用的纯函数(gate)，
#       并用一个 task.json 契约把"任务 -> 门禁判据"串起来。
#
# 单一来源：baseline_check.py 复用本包的判据，避免两处漂移。