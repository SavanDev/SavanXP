/*
 * The FreeType modules libsxfont registers (docs/desktop/FONT_LIBRARY.md):
 * TrueType glyphs, the SFNT tables they come in, and the two rasterizers.
 * cmake/FreeType.cmake points FT_CONFIG_MODULES_H here and compiles exactly
 * these modules; a module listed here and not compiled fails the link.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
