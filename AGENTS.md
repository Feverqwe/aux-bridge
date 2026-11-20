# Aux Bridge

- Read `.local/agent-memory.md` when it exists. Keep machine-specific setup, device names, proxy settings, and private observations under the ignored `.local/` directory. Update the memory when an enduring local decision or a verification finding changes.
- Keep audio callbacks bounded and free of allocation, locks, logging, and calls into Qt. Exercise routing, buffering, resampling, and error handling through observable behavior.
- Keep the README focused on building, running, and the user-visible behavior. Avoid documenting incidental implementation details or testing source text.
- Commit complete, verified iterations when the user authorizes commits. Review staged files for local material before committing. Preserve unrelated work.
