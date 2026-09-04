# Native renderer boundary

The repository retains an independently authored renderer, RHI semantic types,
native pass modules, host resource owners, frame queues, and focused tests. There
is currently no Gears gameplay executable and therefore no live product renderer
path: the named target refuses until `x360port` exposes a Xenia executor.

The first integration must feed authenticated guest memory, device callbacks,
and frame identity from that executor into the existing bounded interfaces. The
compatibility route initially consumes Xenos commands through the retained GPU
frontend. Native RHI replacement is allowed only after same-run semantic state,
resource lifetime, synchronization, shader identity, resolves, presentation, and
pixel output agree with the independent oracle.

Native renderer tests prove local contracts only. Offline frame replay and shader
tools are diagnostic instruments; neither establishes boot, gameplay, or product
performance. The 8.33 ms / 120 fps goal remains unmeasured until representative
interactive gameplay reaches the complete native frontend.
