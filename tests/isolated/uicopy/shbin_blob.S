.section .rodata
.balign 4
.global d9shader_shbin
.hidden d9shader_shbin
.type d9shader_shbin, %object
d9shader_shbin:
.incbin "build/d9shader.shbin"
.global d9shader_shbin_end
.hidden d9shader_shbin_end
.type d9shader_shbin_end, %object
d9shader_shbin_end:
.size d9shader_shbin, d9shader_shbin_end-d9shader_shbin
