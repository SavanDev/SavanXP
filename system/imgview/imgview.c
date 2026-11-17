/* Image Viewer: SUN file/section I/O, SXGUI chrome, libsximg pixels. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sxn/sxn.h"
#include "sxn/sxngui.h"
#include "sxn/sxnfiledlg.h"
#include "savanxp/img.h"
#include "view.h"
#include "imgview.sunres.h"
#include "icons.inc"
#include "../gfx/toolbar_glyphs/icons.inc"

#define IV_PATH SXN_FILE_DIALOG_PATH_MAX
#define IV_MAX_FILE (64u * 1024u * 1024u)
#define IV_CANVAS 0x004e4e4eu
#define IV_WIDTH 960
#define IV_HEIGHT 660

enum { W_OPEN, W_PREVIOUS, W_NEXT, W_IN, W_OUT, W_FIT, W_ACTUAL, W_FULL,
       W_SEP1, W_SEP2, W_SEP3, W_HSCROLL, W_VSCROLL, W_NAME, W_DIMENSIONS, W_FORMAT, W_BYTES, W_ZOOM, W_INDEX, W_COUNT };
enum { C_OPEN=1, C_EXIT, C_PREVIOUS, C_NEXT, C_IN, C_OUT, C_FIT, C_ACTUAL, C_FULL, C_ABOUT };
struct iv_section { long handle; void *data; };
struct iv_image {
    struct iv_section memory;
    struct sx_image_metadata info;
    struct sx_bitmap bitmap;
    uint64_t bytes;
};
static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[W_COUNT];
static struct iv_image g_image;
static struct iv_view g_view;
static struct sx_rect g_canvas;
static struct sx_bitmap g_icons[8];
static const struct sx_bitmap *g_icon_refs[8];
static struct sxgui_dialog g_about;
static struct sxgui_widget g_about_widgets[3];
static char g_path[IV_PATH], g_folder[IV_PATH]="/wallpapers";
static char g_previous[SXN_NAME_MAX], g_next[SXN_NAME_MAX];
static unsigned g_index, g_total;
static int g_listed;
static char g_error[160], g_dimensions[96], g_format[16], g_bytes[40], g_zoom[64], g_position[64];
static int g_prev_left;
static int g_window, g_fullscreen, g_dragging, g_drag_x, g_drag_y, g_drag_scroll_x, g_drag_scroll_y;
static void layout(struct sxngui_app *app);
static void sync_controls(void);
static void show_message(const char *title,const char *first,const char *second);

static struct sxgui_menu_item file_items[]={{"Open...\tCtrl+O",C_OPEN,0},{0,0,0},{"Exit",C_EXIT,0}};
static struct sxgui_menu_item view_items[]={
    {"Previous\tLeft",C_PREVIOUS,0},{"Next\tRight",C_NEXT,0},{0,0,0},
    {"Zoom In\t+",C_IN,0},{"Zoom Out\t-",C_OUT,0},
    {"Fit to Window\tF",C_FIT,0},{"Actual Size\t1",C_ACTUAL,0},{0,0,0},
    {"Full Screen\tF11",C_FULL,0}};
static const struct sxgui_menu_item help_items[]={{"About Image Viewer",C_ABOUT,0}};
static const struct sxgui_menu menus[]={{"File",file_items,3},{"View",view_items,9},{"Help",help_items,1}};
static struct sxgui_menubar g_menubar;

static void section_release(struct iv_section *section)
{
    if (section->data) (void)sxn_unmap_view((uint64_t)(uintptr_t)section->data);
    if (section->handle > 0) (void)sxn_close(section->handle);
    memset(section,0,sizeof(*section));
}
static int section_allocate(struct iv_section *section, uint64_t bytes)
{
    long view;
    section->handle=sxn_section_create(bytes,SXN_SECTION_READ|SXN_SECTION_WRITE);
    if (section->handle < 0) { section->handle=0; return 0; }
    view=sxn_map_view(section->handle,SXN_SECTION_READ|SXN_SECTION_WRITE);
    if (view <= 0) { section_release(section); return 0; }
    section->data=(void *)(uintptr_t)view;
    return 1;
}
static void image_release(struct iv_image *image)
{
    section_release(&image->memory);
    memset(image,0,sizeof(*image));
}
static int io_error(char *error, size_t capacity, const char *operation, long result)
{
    snprintf(error,capacity,"%s (error %ld).",operation,result);
    return -1;
}
/* The same load path is exercised by --selftest and the window. */
static int image_read(struct iv_image *image, const char *path, char *error, size_t capacity)
{
    struct sxn_file_info file;
    struct iv_section input={0};
    const struct sx_image_options options={0};
    long fd=-1,result;
    size_t done=0;
    int loaded=0;
    image_release(image);
    if (!path || path[0]!='/') return io_error(error,capacity,"Use an absolute image path",-SAVANXP_EINVAL);
    result=sxn_query_path(path,&file);
    if (result < 0) return io_error(error,capacity,"Cannot read image information",result);
    if (file.type!=SXN_FILE_TYPE_FILE) return io_error(error,capacity,"The path is not a regular file",-SAVANXP_EISDIR);
    if (!file.size || file.size>IV_MAX_FILE) { snprintf(error,capacity,"Image file is empty or exceeds the 64 MiB limit."); return -1; }
    fd=sxn_open_file(path,SXN_FILE_READ);
    if (fd < 0) return io_error(error,capacity,"Cannot open image",fd);
    if (!section_allocate(&input,file.size)) { snprintf(error,capacity,"Not enough memory for the image file."); goto finish; }
    while (done < file.size) {
        result=sxn_read(fd,(unsigned char *)input.data+done,(unsigned long)(file.size-done));
        if (result<=0) { io_error(error,capacity,"Cannot read the complete image",result); goto finish; }
        done+=(size_t)result;
    }
    result=sx_image_info(input.data,(size_t)file.size,&options,&image->info);
    if (result!=SX_IMAGE_OK) { snprintf(error,capacity,"%s.",sx_image_error_string((int)result)); goto finish; }
    if (!section_allocate(&image->memory,(uint64_t)image->info.width*image->info.height*4u)) {
        snprintf(error,capacity,"Not enough memory for decoded pixels."); goto finish;
    }
    result=sx_image_decode_into(input.data,(size_t)file.size,&options,image->memory.data,
                                (size_t)image->info.width*image->info.height,0);
    if (result!=SX_IMAGE_OK) { snprintf(error,capacity,"%s.",sx_image_error_string((int)result)); goto finish; }
    {
        struct savanxp_fb_info info={0};
        info.width=image->info.width; info.height=image->info.height; info.pitch=info.width*4u;
        info.bpp=32; info.buffer_size=info.pitch*info.height;
        sx_bitmap_wrap(&image->bitmap,image->memory.data,&info,SX_PIXEL_FORMAT_BGRX8888);
    }
    image->bytes=file.size; loaded=1; error[0]=0;
finish:
    (void)sxn_close(fd);
    section_release(&input);
    if (!loaded) image_release(image);
    return loaded?0:-1;
}
static const char *base_name(const char *path)
{
    const char *name=strrchr(path,'/'); return name?name+1:path;
}
static int name_order(const char *a,const char *b)
{
    int order=iv_name_compare(a,b); return order?order:strcmp(a,b);
}
/* Stream the directory instead of truncating a stored list. Folder changes
 * are observed on every open; ordering is case-folded as in Files. */
static void scan_folder(void)
{
    struct sxn_directory_entry entry;
    const char *current=base_name(g_path);
    const char *slash=strrchr(g_path,'/');
    long dir,result;
    size_t length=slash?(size_t)(slash-g_path):0;
    g_previous[0]=g_next[0]=0; g_index=g_total=0; g_listed=0;
    if (length==0) snprintf(g_folder,sizeof(g_folder),"/");
    else { memcpy(g_folder,g_path,length); g_folder[length]=0; }
    dir=sxn_open_directory(g_folder);
    if (dir<0) return;
    while ((result=sxn_read_directory(dir,&entry))>0) {
        int order;
        if (entry.info.type!=SXN_FILE_TYPE_FILE || !iv_image_name(entry.name)) continue;
        ++g_total; order=name_order(entry.name,current);
        if (order<=0) ++g_index;
        if (order==0) g_listed=1;
        if (order<0 && (!g_previous[0] || name_order(entry.name,g_previous)>0))
            snprintf(g_previous,sizeof(g_previous),"%s",entry.name);
        if (order>0 && (!g_next[0] || name_order(entry.name,g_next)<0))
            snprintf(g_next,sizeof(g_next),"%s",entry.name);
    }
    (void)sxn_close(dir);
    if (result<0) { g_previous[0]=g_next[0]=0; g_index=g_total=0; g_listed=0; }
}
static void open_image(const char *path)
{
    char title[IV_PATH+32];
    if (!path || strlen(path)>=sizeof(g_path)) {
        image_release(&g_image); snprintf(g_error,sizeof(g_error),"Image path is too long.");
        g_path[0]=g_previous[0]=g_next[0]=0; g_index=g_total=0; g_listed=0;
    } else {
        /* Dialog paths may share caller storage; preserve before callbacks. */
        memmove(g_path,path,strlen(path)+1);
        (void)image_read(&g_image,g_path,g_error,sizeof(g_error));
        scan_folder();
    }
    memset(&g_view,0,sizeof(g_view)); g_dragging=0;
    g_view.width=(int)g_image.info.width; g_view.height=(int)g_image.info.height;
    g_view.fit=1; g_view.zoom=1000;
    if (g_window) {
        snprintf(title,sizeof(title),"%s - Image Viewer",base_name(g_path));
        (void)sxnwm_set_title(&g_app.wm,title);
        layout(&g_app);
    }
}
static void navigate(int direction)
{
    char path[IV_PATH]; const char *name=direction<0?g_previous:g_next;
    int length;
    if (!name[0]) return;
    length=snprintf(path,sizeof(path),"%s%s%s",g_folder,!strcmp(g_folder,"/")?"":"/",name);
    if (length<0 || (size_t)length>=sizeof(path)) return;
    open_image(path);
}
static void on_file(struct sxngui_app *app,const char *path,void *user)
{ (void)app; (void)user; if (path) open_image(path); }
static void begin_open(void)
{
    struct sxn_file_dialog_request request={0};
    request.mode=SXN_FILE_DIALOG_OPEN; request.title="Open Image"; request.initial_directory=g_folder;
    request.filter_description="Images (*.bmp; *.png; *.jpg; *.jpeg)";
    request.filter_patterns="*.bmp;*.png;*.jpg;*.jpeg"; request.on_done=on_file;
    if (sxn_file_dialog_begin(&g_app,&request)<0) {
        show_message("Image Viewer","Cannot open the file dialog.","Please try again.");
    }
}
static void close_about(struct sxgui_widget *widget,void *user)
{ (void)widget; (void)user; sxgui_dialog_end(&g_app.ui,1); }
static void show_message(const char *title,const char *first,const char *second)
{
    g_about_widgets[0]=sxgui_label(sx_rect_make(14,12,312,22),first);
    g_about_widgets[1]=sxgui_label(sx_rect_make(14,38,312,22),second);
    g_about_widgets[2]=sxgui_button(sx_rect_make(120,76,80,24),"OK",close_about,0);
    memset(&g_about,0,sizeof(g_about)); g_about.title=title;
    g_about.widgets=g_about_widgets; g_about.widget_count=3; g_about.initial_focus=2; g_about.default_button=2;
    sxgui_dialog_begin(&g_app.ui,&g_about,340,114);
}
static void command(int id,void *user)
{
    (void)user;
    switch (id) {
    case C_OPEN: begin_open(); return;
    case C_EXIT: sxngui_app_quit(&g_app,0); return;
    case C_PREVIOUS: navigate(-1); return;
    case C_NEXT: navigate(1); return;
    case C_FULL: (void)sxnwm_set_fullscreen(&g_app.wm,!g_fullscreen); layout(&g_app); return;
    case C_ABOUT:
        show_message("About Image Viewer","Image Viewer - SUN","BMP, PNG and sequential JPEG images"); return;
    default: break;
    }
    if (!g_image.memory.data) return;
    if (id==C_IN || id==C_OUT) iv_view_step(&g_view,id==C_IN?1:-1);
    else if (id==C_ACTUAL) iv_view_zoom(&g_view,1000);
    else if (id==C_FIT) { g_view.fit=1; g_view.x=g_view.y=0; }
    layout(&g_app);
}
static void widget_action(struct sxgui_widget *widget,void *user)
{
    int index=(int)(widget-g_widgets); (void)user;
    if (index==W_HSCROLL || index==W_VSCROLL) {
        iv_view_scroll(&g_view,g_widgets[W_HSCROLL].value,g_widgets[W_VSCROLL].value);
        sxngui_app_request_repaint(&g_app);
    }
}
static void toolbar_action(struct sxgui_widget *widget,void *user)
{
    static const int commands[]={C_OPEN,C_PREVIOUS,C_NEXT,C_IN,C_OUT,C_FIT,C_ACTUAL,C_FULL};
    (void)user; command(commands[widget-g_widgets],0);
}
static void enable(int index,int enabled)
{
    if (enabled) g_widgets[index].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[index].flags |= SXGUI_FLAG_DISABLED;
}
static void sync_controls(void)
{
    const char *format=g_image.info.format==SX_IMAGE_FORMAT_JPEG?"JPEG":
                       g_image.info.format==SX_IMAGE_FORMAT_PNG?"PNG":g_image.info.format==SX_IMAGE_FORMAT_BMP?"BMP":"";
    int loaded=g_image.memory.data!=0;
    enable(W_PREVIOUS,g_previous[0]!=0); enable(W_NEXT,g_next[0]!=0);
    enable(W_IN,loaded && g_view.zoom<16000); enable(W_OUT,loaded && g_view.zoom>125);
    enable(W_FIT,loaded); enable(W_ACTUAL,loaded);
    snprintf(g_dimensions,sizeof(g_dimensions),loaded?"%u x %u pixels":"",
             g_image.info.source_width,g_image.info.source_height);
    snprintf(g_format,sizeof(g_format),"%s",format);
    snprintf(g_bytes,sizeof(g_bytes),loaded?"%llu KiB":"",(unsigned long long)((g_image.bytes+1023)/1024));
    if (loaded) {
        if (g_image.info.scale_denom>1)
            snprintf(g_zoom,sizeof(g_zoom),"%d.%d%% | Decode 1/%u",g_view.zoom/10,g_view.zoom%10,g_image.info.scale_denom);
        else snprintf(g_zoom,sizeof(g_zoom),"%d.%d%%",g_view.zoom/10,g_view.zoom%10);
    } else g_zoom[0]=0;
    if (g_total && g_listed) snprintf(g_position,sizeof(g_position),"%u of %u",g_index,g_total);
    else g_position[0]=0;
    g_widgets[W_NAME].text=g_path[0]?base_name(g_path):"Open an image";
    view_items[0].flags=g_previous[0]?0:SXGUI_MENU_DISABLED;
    view_items[1].flags=g_next[0]?0:SXGUI_MENU_DISABLED;
    for (int i=3;i<=6;++i) view_items[i].flags=loaded?0:SXGUI_MENU_DISABLED;
    if (g_view.fit && loaded) view_items[5].flags|=SXGUI_MENU_CHECKED;
    view_items[8].flags=g_fullscreen?SXGUI_MENU_CHECKED:0;
}
static void layout(struct sxngui_app *app)
{
    int width=(int)app->info.width,height=(int)app->info.height;
    int top,bar_height=sxgui_toolbar_height(),status=SXGUI_STATUS_HEIGHT;
    int hbar=0,vbar=0,x=6,separator=W_SEP1;
    g_fullscreen=(app->wm.header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_FULLSCREEN)!=0;
    sxgui_set_menubar(&app->ui,g_fullscreen?0:&g_menubar);
    top=g_fullscreen?0:sxgui_menubar_height()+bar_height+6;
    for (int i=0;i<8;++i) {
        int button_width=sxgui_toolbar_button_width("Full Screen");
        if (i==1 || i==3 || i==7) {
            g_widgets[separator].rect=sx_rect_make(x+5,sxgui_menubar_height()+6,2,bar_height-8);
            if (g_fullscreen) g_widgets[separator].flags &= ~SXGUI_FLAG_VISIBLE;
            else g_widgets[separator].flags |= SXGUI_FLAG_VISIBLE;
            ++separator; x+=12;
        }
        g_widgets[i].rect=sx_rect_make(x,sxgui_menubar_height()+2,button_width,bar_height);
        if (g_fullscreen) g_widgets[i].flags &= ~SXGUI_FLAG_VISIBLE;
        else g_widgets[i].flags |= SXGUI_FLAG_VISIBLE;
        x+=button_width;
    }
    g_canvas=sx_rect_make(2,top,width-4,height-top-status-4);
    if (g_canvas.width<8) g_canvas.width=8;
    if (g_canvas.height<8) g_canvas.height=8;
    for (int pass=0;pass<3;++pass) {
        int w=g_canvas.width-4-vbar*SXGUI_SCROLLBAR_THICKNESS;
        int h=g_canvas.height-4-hbar*SXGUI_SCROLLBAR_THICKNESS;
        iv_view_resize(&g_view,w,h);
        hbar=g_image.memory.data && !g_view.fit && iv_view_width(&g_view)>w;
        vbar=g_image.memory.data && !g_view.fit && iv_view_height(&g_view)>h;
    }
    g_widgets[W_HSCROLL].rect=sx_rect_make(g_canvas.x+2,g_canvas.y+g_canvas.height-18,g_view.viewport_width,16);
    g_widgets[W_VSCROLL].rect=sx_rect_make(g_canvas.x+g_canvas.width-18,g_canvas.y+2,16,g_view.viewport_height);
    for (int i=W_HSCROLL;i<=W_VSCROLL;++i) {
        int visible=i==W_HSCROLL?hbar:vbar;
        if (visible) g_widgets[i].flags |= SXGUI_FLAG_VISIBLE;
        else g_widgets[i].flags &= ~SXGUI_FLAG_VISIBLE;
        g_widgets[i].range_max=i==W_HSCROLL?iv_view_width(&g_view)-g_view.viewport_width:iv_view_height(&g_view)-g_view.viewport_height;
        if (g_widgets[i].range_max<0) g_widgets[i].range_max=0;
        g_widgets[i].page=i==W_HSCROLL?g_view.viewport_width:g_view.viewport_height;
        g_widgets[i].value=i==W_HSCROLL?g_view.x:g_view.y;
    }
    {
        const int sizes[]={0,170,55,75,185,90};
        x=2;
        for (int i=0;i<6;++i) {
            int w=i==0?width-4-575:sizes[i];
            if (w<24) w=24;
            g_widgets[W_NAME+i].rect=sx_rect_make(x,height-status-2,w,status);
            x+=w;
        }
    }
    sync_controls(); sxngui_app_request_repaint(app);
}
static void paint(struct sxngui_app *app)
{
    struct sx_painter *p=&app->ui.painter;
    struct sx_rect viewport=sx_rect_make(g_canvas.x+2,g_canvas.y+2,g_view.viewport_width,g_view.viewport_height);
    sxgui_draw_sunken_edge(p,g_canvas);
    sx_painter_fill_rect(p,viewport,IV_CANVAS);
    if (!sx_painter_push_clip(p,viewport)) return;
    if (g_image.memory.data) {
        int w=iv_view_width(&g_view),h=iv_view_height(&g_view);
        int x=viewport.x+(w<=viewport.width?(viewport.width-w)/2:-g_view.x);
        int y=viewport.y+(h<=viewport.height?(viewport.height-h)/2:-g_view.y);
        sx_painter_draw_scaled_bitmap(p,&g_image.bitmap,sx_rect_make(x,y,w,h),
            sx_rect_make(0,0,g_view.width,g_view.height),g_view.zoom>=2000?SX_SCALE_NEAREST:SX_SCALE_BILINEAR);
    } else {
        sx_painter_draw_text(p,viewport.x+20,viewport.y+24,g_error[0]?"Unable to open image":"Image Viewer",0x00ffffff);
        sx_painter_draw_text(p,viewport.x+20,viewport.y+48,g_error[0]?g_error:"Choose File > Open or press Ctrl+O.",0x00ffffff);
    }
    sx_painter_pop_clip(p);
}
static int pointer(struct sxngui_app *app,const struct savanxp_gui_pointer_event *event)
{
    int in=sx_rect_contains_point(sx_rect_make(g_canvas.x+2,g_canvas.y+2,g_view.viewport_width,g_view.viewport_height),event->x,event->y);
    int left=(event->buttons&SAVANXP_MOUSE_BUTTON_LEFT)!=0;
    int pressed=left && !g_prev_left;
    g_prev_left=left;
    if (!left) g_dragging=0;
    if (!g_image.memory.data || sxgui_dialog_active(&app->ui) || app->ui.popup_owner>=0 || (app->ui.menubar && app->ui.menubar->open_menu>=0)) return 0;
    if (in && event->wheel) {
        iv_view_scroll(&g_view,g_view.x,g_view.y-event->wheel*48); layout(app); return 1;
    }
    if (pressed && in && !g_dragging) {
        g_dragging=1; g_drag_x=event->x; g_drag_y=event->y; g_drag_scroll_x=g_view.x; g_drag_scroll_y=g_view.y;
    }
    if (g_dragging) {
        iv_view_scroll(&g_view,g_drag_scroll_x+g_drag_x-event->x,g_drag_scroll_y+g_drag_y-event->y);
        layout(app); return 1;
    }
    return 0;
}
static int key(struct sxngui_app *app,const struct savanxp_input_event *event)
{
    if (event->type!=SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(&app->ui)) return 0;
    if (app->ui.popup_owner>=0 || (app->ui.menubar && app->ui.menubar->open_menu>=0)) return 0;
    if ((event->modifiers&SAVANXP_KEY_MOD_CTRL) && (event->ascii=='o'||event->ascii=='O'||event->ascii==15)) { begin_open(); return 1; }
    if (event->key==SAVANXP_KEY_F11) { command(C_FULL,0); return 1; }
    if (event->key==SAVANXP_KEY_ESC && g_fullscreen) { (void)sxnwm_set_fullscreen(&app->wm,0); layout(app); return 1; }
    if (event->key==SAVANXP_KEY_LEFT || event->key==SAVANXP_KEY_PAGE_UP) { navigate(-1); return 1; }
    if (event->key==SAVANXP_KEY_RIGHT || event->key==SAVANXP_KEY_PAGE_DOWN) { navigate(1); return 1; }
    if (event->ascii=='+'||event->ascii=='=') { command(C_IN,0); return 1; }
    if (event->ascii=='-') { command(C_OUT,0); return 1; }
    if (event->ascii=='1') { command(C_ACTUAL,0); return 1; }
    if (event->ascii=='f'||event->ascii=='F') { command(C_FIT,0); return 1; }
    return 0;
}
static void init_icons(void)
{
    const uint32_t *pixels[]={iv_icon_document_open,toolbar_glyph_go_previous,toolbar_glyph_go_next,
        iv_icon_zoom_in,iv_icon_zoom_out,iv_icon_zoom_fit_best,iv_icon_zoom_original,iv_icon_view_fullscreen};
    struct savanxp_fb_info info={0}; info.width=info.height=20; info.pitch=80; info.bpp=32; info.buffer_size=1600;
    for (int i=0;i<8;++i) { sx_bitmap_wrap(&g_icons[i],(uint32_t *)pixels[i],&info,SX_PIXEL_FORMAT_BGRA8888); g_icon_refs[i]=&g_icons[i]; }
}
static int run_window(const char *path)
{
    static const char *labels[]={"Open","Previous","Next","Zoom In","Zoom Out","Fit","Actual Size","Full Screen"};
    int result;
    init_icons();
    for (int i=0;i<8;++i) g_widgets[i]=sxgui_toolbar_button(sx_rect_make(0,0,0,0),labels[i],&g_icon_refs[i],toolbar_action,0);
    for (int i=W_SEP1;i<=W_SEP3;++i) g_widgets[i]=sxgui_separator(sx_rect_make(0,0,0,0));
    g_widgets[W_HSCROLL]=sxgui_scrollbar(sx_rect_make(0,0,0,0),0,0,1,0); g_widgets[W_HSCROLL].flags|=SXGUI_FLAG_HSCROLL;
    g_widgets[W_VSCROLL]=sxgui_scrollbar(sx_rect_make(0,0,0,0),0,0,1,0);
    g_widgets[W_HSCROLL].on_action=g_widgets[W_VSCROLL].on_action=widget_action;
    for (int i=W_NAME;i<W_COUNT;++i) { g_widgets[i]=sxgui_label(sx_rect_make(0,0,0,0),""); g_widgets[i].flags|=SXGUI_FLAG_SUNKEN; }
    g_widgets[W_DIMENSIONS].text=g_dimensions; g_widgets[W_FORMAT].text=g_format; g_widgets[W_BYTES].text=g_bytes;
    g_widgets[W_ZOOM].text=g_zoom; g_widgets[W_INDEX].text=g_position;
    result=sxngui_app_init(&g_app,&sun_resources,g_widgets,W_COUNT);
    if (result<0) { (void)sxn_log("imgview: cannot connect to windowd"); return 1; }
    g_window=1; g_menubar.menus=menus; g_menubar.menu_count=3; g_menubar.on_command=command;
    g_app.on_paint=paint; g_app.on_resize=layout; g_app.on_key=key; g_app.on_pointer=pointer;
    g_view.fit=1; g_view.zoom=1000;
    (void)sxngui_app_set_content_size(&g_app,IV_WIDTH,IV_HEIGHT);
    if (path) open_image(path); else layout(&g_app);
    result=sxngui_app_run(&g_app); image_release(&g_image); g_window=0;
    return result;
}

static void selftest_message(const char *message)
{
    (void)sxn_log(message);
    (void)sxn_write(1,message,strlen(message));
    (void)sxn_write(1,"\n",1);
}
static int selftest(void)
{
    struct iv_image image={0}; char error[160];
    static const char *paths[]={"/imgview-test/01-bitmap.BMP","/imgview-test/02-colour.png","/imgview-test/03-colour.JpEg"};
    int failures=0;
#define CHECK(c, text) do { if (!(c)) { selftest_message("IMGVIEW FAIL: " text); ++failures; } } while (0)
    for (unsigned i=0;i<3;++i) {
        CHECK(image_read(&image,paths[i],error,sizeof(error))==0,"native load of each format");
        CHECK(image.info.format==i+1 && image.info.width==16 && image.info.height==16 &&
              image.info.source_width==16 && image.info.source_height==16 && image.info.scale_denom==1,"image plan metadata");
        if (image.memory.data) {
            const uint32_t *pixels=image.memory.data;
            CHECK(pixels[0]==0xe03050 && pixels[8]==0x20d060 && pixels[8*16]==0x3050e0 &&
                  pixels[8*16+8]==0xe0d030,"exact opaque pixels from BMP PNG JPEG");
        }
    }
    CHECK(image_read(&image,"/imgview-test/04-large.jpg",error,sizeof(error))==0 && image.info.width==2049 &&
          image.info.height==9 && image.info.scale_denom==2 && image.info.source_width==4097,"JPEG reduction through the actual loader");
    CHECK(image_read(&image,"/imgview-test/06-progressive.jpg",error,sizeof(error))<0 && strstr(error,"Progressive"),"named unsupported encoding");
    CHECK(!image.memory.data && !image.memory.handle && !image.info.width,"failed load leaves no stale pixels");
    CHECK(image_read(&image,"/imgview-test/07-broken.jpg",error,sizeof(error))<0,"valid header missing entropy end rejected");
    CHECK(!image.memory.data && !image.memory.handle,"late failure releases pixel section");
    CHECK(image_read(&image,"/imgview-test/08-alpha.png",error,sizeof(error))==0 &&
          image.memory.data && ((uint32_t *)image.memory.data)[0]==0x7f7fff,"alpha composites onto white");
    image_release(&image);
    CHECK(image_read(&image,"/imgview-test/missing.png",error,sizeof(error))<0,"missing file error");
    CHECK(image_read(&image,"relative.png",error,sizeof(error))<0,"relative paths require the dialog or an absolute argument");
    CHECK(image_read(&image,"/imgview-test",error,sizeof(error))<0,"directory is not an image");
    open_image("/imgview-test/03-colour.JpEg");
    CHECK(g_total==8 && g_index==3 && !strcmp(g_previous,"02-colour.png") && !strcmp(g_next,"04-large.jpg"),"folder navigation sorted as Files");
    navigate(-1); CHECK(g_image.info.format==SX_IMAGE_FORMAT_PNG && g_index==2,"previous image");
    navigate(1); CHECK(g_image.info.format==SX_IMAGE_FORMAT_JPEG && g_index==3,"next image");
    open_image("/imgview-test/01-bitmap.BMP"); navigate(-1);
    CHECK(g_index==1 && !g_previous[0] && g_image.info.format==SX_IMAGE_FORMAT_BMP,"navigation stops at the first image");
    open_image("/imgview-test/08-alpha.png"); navigate(1);
    CHECK(g_index==8 && !g_next[0] && g_image.info.format==SX_IMAGE_FORMAT_PNG,"navigation stops at the last image");
    image_release(&g_image);
#undef CHECK
    selftest_message(failures?"IMGVIEW SELFTEST FAIL":"IMGVIEW SELFTEST PASS"); return failures?1:0;
}
int main(int argc,char **argv)
{
    if (argc>1 && !strcmp(argv[1],"--selftest")) return selftest();
    return run_window(argc>1?argv[1]:0);
}
