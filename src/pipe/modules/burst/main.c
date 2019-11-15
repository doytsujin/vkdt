#include "modules/api.h"

// TODO: redo timing. but it seems down4 is a lot more precise (less pyramid approx?)
#define DOWN 4
#if DOWN==4
#define NUM_LEVELS 4
#else
#define NUM_LEVELS 6
#endif

// the roi callbacks are only needed for the debug outputs. other than that
// the default implementation would work fine for us.
void modify_roi_in(
    dt_graph_t *graph,
    dt_module_t *module)
{
  for(int i=0;i<3+NUM_LEVELS*2;i++)
  {
    module->connector[i].roi.wd = module->connector[i].roi.full_wd;
    module->connector[i].roi.ht = module->connector[i].roi.full_ht;
    module->connector[i].roi.scale = 1.0f;
  }
}

void modify_roi_out(
    dt_graph_t *graph,
    dt_module_t *module)
{
  const int block = module->img_param.filters == 9u ? 3 : 2;
  module->connector[2].roi = module->connector[0].roi;
  for(int i=0;i<NUM_LEVELS;i++)
  {
    module->connector[3+i].roi = module->connector[3+i-1].roi;
    int scale = i == 0 ? block : DOWN;
    module->connector[3+i].roi.full_wd /= scale;
    module->connector[3+i].roi.full_ht /= scale;
    module->connector[7+i] = module->connector[3+i];
  }
}

// input connectors: fixed raw file
//                   to-be-warped raw file
// output connector: warped raw file
void
create_nodes(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  int id_debug = -1;
  int cn_debug = -1;
  // connect each mosaic input to half, generate grey lum map for both input images
  // by a sequence of half, down4, down4, down4 kernels.
  // then compute distance (dist kernel) coarse to fine, merge best offsets (merge kernel),
  // input best coarse offsets to next finer level, and finally output offsets on finest scale.
  //
  dt_roi_t roi[NUM_LEVELS+1] = {module->connector[0].roi};
  const int block = module->img_param.filters == 9u ? 3 : 2;
  for(int i=1;i<=NUM_LEVELS;i++)
  {
    // int scale = i == 1 ? block : 4;
    int scale = i == 1 ? block : DOWN;
    roi[i] = roi[i-1];
    roi[i].full_wd /= scale;
    roi[i].full_ht /= scale;
    roi[i].wd /= scale;
    roi[i].ht /= scale;
    roi[i].x  /= scale;
    roi[i].y  /= scale;
  }

  int id_down[2][NUM_LEVELS] = {0};
  for(int k=0;k<2;k++)
  {
    assert(graph->num_nodes < graph->max_nodes);
    id_down[k][0] = graph->num_nodes++;
    graph->node[id_down[k][0]] = (dt_node_t) {
      .name   = dt_token("burst"),
      .kernel = dt_token("half"),
      .module = module,
      .wd     = roi[1].wd,
      .ht     = roi[1].ht,
      .dp     = 1,
      .num_connectors = 2,
      .connector = {{
        .name   = dt_token("input"),
        .type   = dt_token("read"),
        .chan   = dt_token("rggb"),
        .format = module->connector[k].format,
        .roi    = roi[0],
        .connected_mi = -1,
      },{
        .name   = dt_token("output"),
        .type   = dt_token("write"),
        .chan   = dt_token("y"),
        .format = dt_token("f16"),
        .roi    = roi[1],
      }},
      .push_constant_size = 4,
      .push_constant = { module->img_param.filters },
    };
    dt_connector_copy(graph, module, k, id_down[k][0], 0);

    for(int i=1;i<NUM_LEVELS;i++)
    {
      assert(graph->num_nodes < graph->max_nodes);
      id_down[k][i] = graph->num_nodes++;
      graph->node[id_down[k][i]] = (dt_node_t) {
        .name   = dt_token("burst"),
#if DOWN==4
        .kernel = dt_token("down4"),
#else
        .kernel = dt_token("down2"),
#endif
        .module = module,
        .wd     = roi[i+1].wd,
        .ht     = roi[i+1].ht,
        .dp     = 1,
        .num_connectors = 2,
        .connector = {{
          .name   = dt_token("input"),
          .type   = dt_token("read"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i],
          .connected_mi = -1,
        },{
          .name   = dt_token("output"),
          .type   = dt_token("write"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
        }},
      };
      CONN(dt_node_connect(graph, id_down[k][i-1], 1, id_down[k][i], 0));
    }
  }

  // XXX DEBUG:
  int id_off[NUM_LEVELS] = {0};

  // connect uv-diff and merge:
  int id_offset = -1;
  for(int i=NUM_LEVELS-1;i>=0;i--) // all depths/coarseness levels, starting at coarsest
  {
    int id_merged = -1;
    // XXX is it faster to do -1..1 and /2 instead of -2..2 and /4?
#if DOWN==4
    const int scale = 4;
    const int vm = -2; // -4;
    const int vM =  3; //  5;
    const int um = -2; // -4;
    const int uM =  3; //  5;
#else
    // scale is to next *coarser* level so we can access id_offset!
    const int scale = 2;
    const int vm = -2;//-1;
    const int vM =  3;//2;
    const int um = -2;//-1;
    const int uM =  3;//2;
#endif
    for(int v=vm;v<vM;v++) for(int u=um;u<uM;u++)
    { // for all offsets in search window
      assert(graph->num_nodes < graph->max_nodes);
      int id_dist = graph->num_nodes++;
      graph->node[id_dist] = (dt_node_t) {
        .name   = dt_token("burst"),
        .kernel = dt_token("dist"),
        .module = module,
        .wd     = roi[i+1].wd,
        .ht     = roi[i+1].ht,
        .dp     = 1,
        .num_connectors = 4,
        .connector = {{
          .name   = dt_token("input"),
          .type   = dt_token("read"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
          .connected_mi = -1,
        },{
          .name   = dt_token("warped"),
          .type   = dt_token("read"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
          .connected_mi = -1,
        },{
          .name   = dt_token("offset"),
          .type   = dt_token("read"),
          .chan   = id_offset >= 0 ? dt_token("rgb") : dt_token("y"),
          .format = dt_token("f16"),
          .roi    = id_offset >= 0 ? roi[i+2] : roi[i+1],
          .flags  = s_conn_smooth,
          .connected_mi = -1,
        },{
          .name   = dt_token("output"),
          .type   = dt_token("write"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
        }},
        .push_constant_size = sizeof(uint32_t)*3,
        .push_constant = { u, v, id_offset >= 0 ? scale : 0 },
      };
      CONN(dt_node_connect(graph, id_down[0][i], 1, id_dist, 0));
      CONN(dt_node_connect(graph, id_down[1][i], 1, id_dist, 1));
      if(id_offset >= 0)
        CONN(dt_node_connect(graph, id_offset, 3, id_dist, 2));
      else // need to connect a dummy
        CONN(dt_node_connect(graph, id_down[0][i], 1, id_dist, 2));

      // blur output of dist node by tile size (depending on noise 16x16, 32x32 or 64x64?)
      const int id_blur = dt_api_blur(graph, module, id_dist, 3, 16);

      int first_time = 0;
      if(id_merged < 0 && id_offset < 0) first_time = 3;
      if(id_merged >=0 && id_offset < 0) first_time = 2;
      if(id_merged < 0 && id_offset >=0) first_time = 1;
      // merge output of blur node using "merge" (<off0, <off1, >merged)
      assert(graph->num_nodes < graph->max_nodes);
      const int id_merge = graph->num_nodes++;
      graph->node[id_merge] = (dt_node_t) {
        .name   = dt_token("burst"),
        .kernel = dt_token("merge"),
        .module = module,
        .wd     = roi[i+1].wd,
        .ht     = roi[i+1].ht,
        .dp     = 1,
        .num_connectors = 4,
        .connector = {{
          .name   = dt_token("dist"),
          .type   = dt_token("read"),
          .chan   = dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
          .connected_mi = -1,
        },{
          .name   = dt_token("coff"),
          .type   = dt_token("read"),
          .chan   = id_offset >= 0 ? dt_token("rgb") : dt_token("y"),
          .format = dt_token("f16"),
          .roi    = id_offset >= 0 ? roi[i+2] : roi[i+1],
          .flags  = s_conn_smooth,
          .connected_mi = -1,
        },{
          .name   = dt_token("loff"),
          .type   = dt_token("read"),
          .chan   = id_merged >= 0 ? dt_token("rgb") : dt_token("y"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
          .connected_mi = -1,
        },{
          .name   = dt_token("merged"),
          .type   = dt_token("write"),
          .chan   = dt_token("rgb"),
          .format = dt_token("f16"),
          .roi    = roi[i+1],
        }},
        .push_constant_size = 4*sizeof(uint32_t),
        .push_constant = { u, v, first_time, scale },  // first time does not have valid off1 as input
      };
      id_off[i] = id_merge;

      CONN(dt_node_connect(graph, id_blur, 1, id_merge, 0));
      // connect coarse offset buffer from previous level:
      if(id_offset >= 0)
        CONN(dt_node_connect(graph, id_offset, 3, id_merge, 1));
      else // need to connect a dummy
        CONN(dt_node_connect(graph, id_blur, 1, id_merge, 1));
      // connect previously merged offsets from same level:
      if(id_merged >= 0)
        CONN(dt_node_connect(graph, id_merged, 3, id_merge, 2));
      else // connect dummy
        CONN(dt_node_connect(graph, id_blur, 1, id_merge, 2));
      id_merged = id_merge;
      // last iteration: remember our merged output as offset for next finer scale:
      if(u == vM-1 && v == vM-1) id_offset = id_merge;
    }
    // id_offset is the final merged offset buffer on this level, ready to be
    // used as input by the next finer level, if any
  }
  // id_offset is now our finest offset buffer, to be used to warp the second
  // raw image to match the first.
  // note that this buffer has dimensions roi[1], i.e. it is still half sized as compared
  // to the full raw image. it is thus large enough to move around full bayer/xtrans blocks,
  // but would need refinement if more fidelity is required (as input for
  // splatting super res demosaic for instance)

#if 1 // DEBUG: connect pyramid debugging output images
  for(int i=0;i<NUM_LEVELS;i++)
  {
    // connect unwarped input buffer, downscaled:
    dt_connector_copy(graph, module, 3+i, id_down[0][i], 1);
    if(i==NUM_LEVELS-1)
    {
      dt_connector_copy(graph, module, 7+i, id_down[1][i], 1);
      continue;
    }
    // connect warp node and warp downscaled buffer:
    assert(graph->num_nodes < graph->max_nodes);
    const int id_warp = graph->num_nodes++;
    graph->node[id_warp] = (dt_node_t) {
      .name   = dt_token("burst"),
      .kernel = dt_token("warp"),
      .module = module,
      .wd     = roi[i+1].wd,
      .ht     = roi[i+1].ht,
      .dp     = 1,
      .num_connectors = 3,
      .connector = {{
        .name   = dt_token("input"),
        .type   = dt_token("read"),
        .chan   = dt_token("y"),
        .format = dt_token("f16"),
        .roi    = roi[i+1],
        .connected_mi = -1,
      },{
        .name   = dt_token("offset"),
        .type   = dt_token("read"),
        .chan   = dt_token("rgb"),
        .format = dt_token("f16"),
        .roi    = roi[i+2],
        .flags  = s_conn_smooth,
        .connected_mi = -1,
      },{
        .name   = dt_token("output"),
        .type   = dt_token("write"),
        .chan   = dt_token("y"),
        .format = dt_token("f16"),
        .roi    = roi[i+1],
      }},
      .push_constant_size = sizeof(uint32_t),
      .push_constant = { 0 },
    };
    CONN(dt_node_connect(graph, id_down[1][i], 1, id_warp, 0));
    CONN(dt_node_connect(graph, id_off[i+1],   3, id_warp, 1));
    dt_connector_copy(graph, module, 7+i, id_warp, 2);
  }
#endif

  assert(graph->num_nodes < graph->max_nodes);
  const int id_warp = graph->num_nodes++;
  graph->node[id_warp] = (dt_node_t) {
    .name   = dt_token("burst"),
    .kernel = dt_token("warp"),
    .module = module,
    .wd     = roi[0].wd,
    .ht     = roi[0].ht,
    .dp     = 1,
    .num_connectors = 3,
    .connector = {{
      .name   = dt_token("input"),
      .type   = dt_token("read"),
      .chan   = dt_token("y"),
      .format = dt_token("f16"),
      .roi    = roi[0],
      .connected_mi = -1,
    },{
      .name   = dt_token("offset"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgb"),
      .format = dt_token("f16"),
      .roi    = roi[1],
      .flags  = s_conn_smooth,
      .connected_mi = -1,
    },{
      .name   = dt_token("output"),
      .type   = dt_token("write"),
      .chan   = dt_token("rggb"),
      .format = dt_token("f16"),
      .roi    = roi[0],
    }},
    .push_constant_size = sizeof(uint32_t),
    .push_constant = { module->img_param.filters },
  };
  dt_connector_copy(graph, module, 1, id_warp, 0);
  CONN(dt_node_connect(graph, id_offset, 3, id_warp, 1));
  dt_connector_copy(graph, module, 2, id_warp, 2);
  if(id_debug >= 0)
  {
    graph->node[id_warp].connector[1].chan   = graph->node[id_debug].connector[cn_debug].chan;
    graph->node[id_warp].connector[1].format = graph->node[id_debug].connector[cn_debug].format;
    CONN(dt_node_connect(graph, id_debug, cn_debug, id_warp, 1));
  }
#undef NUM_LEVELS
}