"""AMD backend: HIP neural pipeline (AMD cards, Phase 2).

Native HIP host and kernels live in amd_mode/native/; the Python side
(parameter mapping, BYO weights management) in amd_mode/python/.
Derived third-party code stays under amd_mode/third_party/ under its own
licence (see THIRD_PARTY_NOTICES.md at the root).
"""
