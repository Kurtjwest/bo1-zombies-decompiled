#pragma once
#include <universal/ptr32.h>
#include <universal/bo1_layout.h>
#include "rigid_body.h"
#include <gfx_d3d/r_workercmds.h>
#include <tl/base/tl_thread.h>

struct broad_phase_memory_info // sizeof=0xC
{                                       // XREF: ?Phys_Init@@YAXXZ/r
    int m_max_num_gjk_ci;               // XREF: Phys_Init(void)+88C/w
    int m_max_num_sap_active_pairs;     // XREF: Phys_Init(void)+893/w
    int m_max_num_surface_types;        // XREF: Phys_Init(void)+89A/w

    broad_phase_memory_info()
    {
        this->m_max_num_gjk_ci = 0;
        this->m_max_num_sap_active_pairs = 0;
        this->m_max_num_surface_types = 0;
    }
};

struct broad_phase_prolog_task_input // sizeof=0x8
{                                       // XREF: ?broad_phase_process_object_environment_collision@@YAXAAVbpi_environment_collision_info@@@Z/r
    Ptr32<phys_vec3> m_aabb_min;
    Ptr32<phys_vec3> m_aabb_max;
};

struct __declspec(align(8)) broad_phase_base // sizeof=0x50
{                                                                             // XREF: broad_phase_info/r
    enum
    {
        FLAG_IS_BPI = 1,
        FLAG_IS_BPG = 2,
        FLAG_IS_BPI_ENV = 4,
        FLAG_IS_IN_CLUSTER = 16
    };

    phys_vec3 m_trace_aabb_min_whace;
    phys_vec3 m_trace_aabb_max_whace;
    phys_vec3 m_trace_translation;
    unsigned int m_flags;
    Ptr32<broad_phase_base> m_list_bpb_next;
    Ptr32<broad_phase_base> m_list_bpb_cluster_next;
    Ptr32<void> m_sap_node;
    Ptr32<void> m_user_data;
    unsigned int m_env_collision_flags;
    unsigned int m_my_collision_type_flags;
    // zombies (port): the exe's 4 padding bytes, as a member. Itanium C++ ABI targets place a derived class's first
    // members in a non-POD base's tail padding, MSVC never does: without this broad_phase_info::m_rb sat at 0x4C
    // instead of 0x50, and the decompiled container math (&bpi[-1].m_gjk_geom: the phys_free_list node of a
    // broad_phase_info) pointed 4 bytes before the node (physics slot pool assert when a ragdoll's bodies were freed).
    unsigned int m_msvc_tail_padding;

    int get_flag(int flag)
    {
        return this->m_flags & flag;
    }

    bool is_bpi()
    {
        return (this->m_flags & FLAG_IS_BPI) != 0;
    }

    bool is_bpi_env()
    {
        return (this->m_flags & FLAG_IS_BPI_ENV) != 0;
    }

    bool is_bpg()
    {
        return (this->m_flags & FLAG_IS_BPG) != 0;
    }

    bool is_in_cluster()
    {
        return (this->m_flags & FLAG_IS_IN_CLUSTER) != 0;
    }

    broad_phase_base *get_bpb_cluster_next()
    {
        return m_list_bpb_cluster_next;
    }

    struct broad_phase_info *get_bpi()
    {
        return (broad_phase_info *)this;
    }

    struct broad_phase_group *get_bpg()
    {
        return (broad_phase_group *)this;
    }

    struct broad_phase_info *get_bpi_env()
    {
        return get_bpi();
    }

    void get_aabb(phys_vec3 *aabb);
};

struct rb_vehicle_model // sizeof=0x90
{
    phys_vec3 m_right_dir_loc;
    phys_vec3 m_forward_dir_loc;
    phys_static_array<rigid_body_constraint_wheel *, 4> m_wheels;
    float m_desired_speed_factor;
    float m_acceleration_factor;
    float m_power_braking_factor;
    float m_braking_factor;
    float m_coasting_factor;
    float m_reference_wheel_radius;
    float m_steer_factor;
    float m_steer_factor_offset;
    float m_steer_current_angle;
    float m_steer_max_angle;
    float m_steer_speed;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    phys_vec3 m_steer_front_pt_loc;
    float m_steer_front_back_length;
    unsigned int m_state_flags;
    Ptr32<rigid_body_constraint_upright> m_rbc_upright;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct broad_phase_group : broad_phase_base // sizeof=0x60
{                                       // XREF: phys_free_list<broad_phase_group>::T_internal/r
    Ptr32<broad_phase_info> m_list_bpi_head;
    int m_bpi_count;
    Ptr32<rb_vehicle_model> m_rbvm;
    Ptr32<struct phys_wheel_collide_info> m_list_wci;

    void set();
    void add_bpi(broad_phase_info *bpi);
    void collision_prolog();
    void collision_epilog();
};
// zombies (port): the exe's layout (see broad_phase_base::m_msvc_tail_padding)
static_assert(sizeof(broad_phase_base) == 0x50, "broad_phase_base: the exe's size");
static_assert(offsetof(broad_phase_group, m_list_bpi_head) == 0x50 && sizeof(broad_phase_group) == 0x60,
              "broad_phase_group: the exe's layout");

const struct __declspec(align(16)) broad_phase_environment_query_input // sizeof=0x40
{                                       // XREF: ?bp_env_jq_batch_function2@@YAHPAUjqBatch@@@Z/r
    phys_vec3 trace_aabb_min_wace;
    phys_vec3 trace_aabb_max_wace;
    phys_vec3 trace_translation;
    unsigned int env_collision_flags;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct broad_phase_base_list // sizeof=0x8
{                                       // XREF: broad_phase_environement_query_results/r
    struct node // sizeof=0x8
    {
        Ptr32<broad_phase_base> m_bpb;
        Ptr32<broad_phase_base_list::node> m_next;
    };
    Ptr32<broad_phase_base_list::node> m_list;
    Ptr32<Ptr32<broad_phase_base_list::node>> m_list_cur;

    void add(broad_phase_base *bpb);
};

struct broad_phase_terrain_query_callback // sizeof=0x4
{                                       // XREF: standard_query/r
    //broad_phase_terrain_query_callback_vtbl *__vftable;
                                        // XREF: _dynamic_initializer_for__g_standard_query__+3/w
                                        // _dynamic_initializer_for__g_standard_query__+D/w
    //struct /*VFT*/ broad_phase_terrain_query_callback_vtbl // sizeof=0x4
    //{
    //    void (__thiscall *query)(broad_phase_terrain_query_callback *this, const broad_phase_environment_query_input *, broad_phase_environement_query_results *);
    //};
    // Defined inline: the base is never used on its own (standard_query overrides it), but the Itanium ABI emits the
    // vtable and typeinfo alongside the first non-inline virtual function, which never had a definition.
    virtual void query(const broad_phase_environment_query_input *, broad_phase_environement_query_results *) {}
};

struct broad_phase_collision_pair // sizeof=0xC
{                                       // XREF: phys_free_list<broad_phase_collision_pair>::T_internal/r
    Ptr32<broad_phase_info> m_bpi1;
    Ptr32<broad_phase_info> m_bpi2;
    Ptr32<broad_phase_collision_pair> m_next_bpcp;
};

struct broad_phase_memory // sizeof=0xCD8
{
    tlAtomicMutex g_bp_auto_activate_mutex;
    tlAtomicReadWriteMutex g_bp_gjk_cache_mutex;
    phys_heap_gjk_cache_system_avl_tree g_phys_gjk_cache_system;
    phys_free_list<broad_phase_info> g_list_broad_phase_info;
    phys_free_list<broad_phase_group> g_list_broad_phase_group;
    phys_free_list<broad_phase_collision_pair> g_list_broad_phase_collision_pair;
    bpei_database_t g_bpei_database;
    Ptr32<broad_phase_terrain_query_callback> g_broad_phase_terrain_query_callback;
    Ptr32<broad_phase_base> g_list_bpb;
    int g_list_bpb_count;
    Ptr32<broad_phase_info> m_list_bpi_env;
    volatile unsigned int m_bpi_env_count;
    int m_bpg_env_count;
    int m_bpg_env_bpi_count;
    int m_bpi_env_no_database_count;
    int m_memory_high_water;
    phys_link_list<phys_collision_pair> g_list_phys_collide_data;
    phys_transient_allocator g_collision_memory_buffer;
    Ptr32<struct phys_surface_type_info> g_surface_type_info_database;
    int m_max_num_surface_types;
    int m_max_num_surface_type_infos;

    broad_phase_memory() = default; // verified all child constructors
    ~broad_phase_memory() = default; // verified all child destructors

    void list_bpb_remove(broad_phase_base *bpb_to_remove);
    static broad_phase_memory *allocate_buffer(const broad_phase_memory_info *bpmi);
};
BO1_LAYOUT32_ASSERT(sizeof(broad_phase_memory) == 0xCD8);

struct bpi_environment_collision_info // sizeof=0x10
{                                       // XREF: ?broad_phase_process_object_environment_collision@@YAXXZ/r
    Ptr32<broad_phase_base> m_bpb_i_start;    // XREF: broad_phase_process_object_environment_collision(void)+17/w
                                        // broad_phase_process_object_environment_collision(void)+39/r ...
    Ptr32<broad_phase_base> m_bpb_i_end;      // XREF: broad_phase_process_object_environment_collision(void)+1A/w
                                        // broad_phase_process_object_environment_collision(void)+3C/w
    int m_bpb_count;                    // XREF: broad_phase_process_object_environment_collision(void)+21/w
                                        // broad_phase_process_object_environment_collision(void)+5B/w
    int m_bpb_last_count;               // XREF: broad_phase_process_object_environment_collision(void)+24/w
                                        // broad_phase_process_object_environment_collision(void)+55/r ...
};

struct __declspec(align(16)) phys_wheel_collide_info // sizeof=0x40
{
    phys_vec3 m_ray_pos;
    phys_vec3 m_ray_dir;
    phys_vec3 m_hitn;
    float m_hit_t;
    Ptr32<broad_phase_info> m_hit_bpi;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    // padding byte

    void collision_process(broad_phase_info *bpi);

    void collision_epilog(struct rigid_body_constraint_wheel *rbc_wheel);
};

struct broad_phase_info *create_broad_phase_info(void);

void    comp_trace_volume(
                const phys_vec3 *aabb1_min,
                const phys_vec3 *aabb1_max,
                const phys_vec3 *aabb2_min,
                const phys_vec3 *aabb2_max,
                phys_vec3 *p1,
                phys_vec3 *p2,
                phys_vec3 *half_dims);


struct axis_aligned_sweep_and_prune // sizeof=0x28
{
    struct sap_node;
    struct axis_element // sizeof=0x14
    {                                       // XREF: axis_aligned_sweep_and_prune::sap_node/r
        Ptr32<axis_aligned_sweep_and_prune::sap_node> m_node;
        int m_min_max;
        float m_val;
        int m_ae_list_index;
        Ptr32<axis_aligned_sweep_and_prune::axis_element> m_next;
    };
    struct sap_node // sizeof=0x80
    {
        Ptr32<broad_phase_base> m_bpb;
        axis_aligned_sweep_and_prune::axis_element m_ae1[3][2];
        int m_updated;


        void init(
            broad_phase_base *bpb,
            Ptr32<axis_aligned_sweep_and_prune::axis_element> *xlist,
            Ptr32<axis_aligned_sweep_and_prune::axis_element> *ylist,
            Ptr32<axis_aligned_sweep_and_prune::axis_element> *zlist);
        void update_ae_val();
    };
    struct active_pair // sizeof=0x10
    {
        Ptr32<axis_aligned_sweep_and_prune::sap_node> m_p1;
        Ptr32<axis_aligned_sweep_and_prune::sap_node> m_p2;
        Ptr32<axis_aligned_sweep_and_prune::active_pair> m_next;
        Ptr32<phys_gjk_cache_info> m_gjk_ci;
    };
    phys_simple_allocator<axis_aligned_sweep_and_prune::sap_node> m_sap_node_allocator;
    Ptr32<axis_aligned_sweep_and_prune::axis_element> m_x_list;
    Ptr32<axis_aligned_sweep_and_prune::axis_element> m_y_list;
    Ptr32<axis_aligned_sweep_and_prune::axis_element> m_z_list;
    Ptr32<bool(const broad_phase_base *, const broad_phase_base *)> m_should_collide_callback;
    phys_simple_allocator<axis_aligned_sweep_and_prune::active_pair> m_active_pair_allocator;
    int m_max_num_active_pairs;
    Ptr32<axis_aligned_sweep_and_prune::active_pair> m_list_bpi_bpi;
    Ptr32<axis_aligned_sweep_and_prune::active_pair> m_list_bpi_bpg;
    Ptr32<axis_aligned_sweep_and_prune::active_pair> m_list_bpg_bpg;


    bool are_overlapping(axis_aligned_sweep_and_prune::sap_node *n1, axis_aligned_sweep_and_prune::sap_node *n2);
    void remove(Ptr32<axis_aligned_sweep_and_prune::axis_element> *axis_list, axis_aligned_sweep_and_prune::sap_node *node);
    void swap(Ptr32<axis_aligned_sweep_and_prune::axis_element> *a1_ptr, Ptr32<axis_aligned_sweep_and_prune::axis_element> *a2_ptr);
    void merge_sort(Ptr32<axis_aligned_sweep_and_prune::axis_element> *list, int list_count);
    void init_system(int max_num_active_pairs);
    void add_active_pair(axis_aligned_sweep_and_prune::sap_node *p1, axis_aligned_sweep_and_prune::sap_node *p2);
    void create_sap_node(broad_phase_base *bpb);
    void remove(Ptr32<axis_aligned_sweep_and_prune::active_pair> *list_ap, axis_aligned_sweep_and_prune::sap_node *node);
    void destroy_sap_node(broad_phase_base *bpb);
    void process_active_pair_list();
    void process();
};
BO1_LAYOUT32_ASSERT(sizeof(axis_aligned_sweep_and_prune) == 40);

struct __declspec(align(8)) broad_phase_info : broad_phase_base // sizeof=0x70
{                                                                             // XREF: phys_free_list<broad_phase_info>::T_internal/r
    Ptr32<rigid_body> m_rb;
    Ptr32<const phys_mat44> m_rb_to_world_xform;
    Ptr32<const phys_mat44> m_cg_to_world_xform;
    Ptr32<const phys_mat44> m_cg_to_rb_xform;
    Ptr32<const struct phys_gjk_geom> m_gjk_geom;
    unsigned int m_gjk_geom_id;
    int m_surface_type;
    // padding byte
    // padding byte
    // padding byte
    // padding byte

    void set_bpi_env(phys_auto_activate_callback *auto_activate_callback);

    void set(
        rigid_body *rb,
        const phys_mat44 *rb_to_world_xform,
        const phys_mat44 *cg_to_world_xform,
        const phys_mat44 *cg_to_rb_xform,
        const phys_gjk_geom *gjk_geom,
        unsigned int gjk_geom_id,
        bool calc_cg_to_world_xform,
        int surface_type,
        void *user_data,
        unsigned int env_collision_flags);

    void collision_prolog();
};
// zombies (port): the exe's layout, which destroy_broad_phase_info(_list)'s &bpi[-1].m_gjk_geom relies on
static_assert(offsetof(broad_phase_info, m_rb) == 0x50 && offsetof(broad_phase_info, m_gjk_geom) == 0x60
                  && sizeof(broad_phase_info) == 0x70,
              "broad_phase_info: the exe's layout");

// "environement". for an authentic decompilation experience. (fixed in blops2!!).
struct broad_phase_environement_query_results // sizeof=0x14
{                                       // XREF: ?bp_env_jq_batch_function2@@YAHPAUjqBatch@@@Z/r
    broad_phase_base_list m_list_bpi_env;
    int m_list_bpi_env_count;
    int m_thread_id;                    // XREF: bp_env_jq_batch_function2(jqBatch *)+10/w
    unsigned int m_env_collision_flags;

    inline void add(broad_phase_base *bpb)
    {
        broad_phase_info *bpi_env; // [esp+4h] [ebp-34h]

        if ((bpb->m_flags & 4) == 0
            && _tlAssert(
                "C:\\projects_pc\\cod\\codsrc\\tl\\physics\\include\\collision\\phys_broad_phase.h",
                389,
                "bpb->is_bpi_env()",
                ""))
        {
            __debugbreak();
        }
        //bpi_env = broad_phase_base::get_bpi_env(bpb);
        bpi_env = bpb->get_bpi_env();
        //broad_phase_base_list::add(&this->m_list_bpi_env, bpi_env);
        this->m_list_bpi_env.add((broad_phase_base *)bpi_env);
        ++this->m_list_bpi_env_count;
        this->m_env_collision_flags |= bpi_env->m_env_collision_flags;
    }
};

struct standard_query : broad_phase_terrain_query_callback // sizeof=0x4
{                                       // XREF: .data:standard_query g_standard_query/r

    void query(const broad_phase_environment_query_input *bpeqi, broad_phase_environement_query_results *bpeqr);
};

void init_bpeqi(broad_phase_environment_query_input *bpeqi, broad_phase_base *bpb);
void check_terrain_query_params(broad_phase_base *bpb);
void calc_largest_vel_sq(broad_phase_info *bpi);
void broad_phase_reset_buffer();
int  bp_env_jq_batch_function1(jqBatch *pBatch);
void merge_sort_bpb(struct broad_phase_base **list, int list_count);
void broad_phase_process_object_environment_collision(bpi_environment_collision_info *eci);
void broad_phase_process_object_environment_collision();

void aasap_list_add(broad_phase_base *bpb);
void aasap_list_remove(broad_phase_base *bpb);
void do_initial_tunnel_test(
                broad_phase_group *bpg,
                const broad_phase_environement_query_results *bpeqr);
void destroy_broad_phase_info(broad_phase_info *bpi);
void destroy_broad_phase_info_list(broad_phase_info *list_bpi);
broad_phase_collision_pair *create_broad_phase_collision_pair();
void destroy_broad_phase_collision_pair(broad_phase_collision_pair *bpcp);
void destroy_broad_phase_collision_pair_list(broad_phase_collision_pair *list_bpcp);
broad_phase_group *create_broad_phase_group();
void destroy_broad_phase_group(broad_phase_group *bpg);
char    bpi_do_gjk_intersect(
                broad_phase_info *p1,
                broad_phase_info *p2,
    float hit_time);

void collide_bpi_environment(broad_phase_info *bpi, const broad_phase_environement_query_results &bpeqr);
void collide_bpg_environment(broad_phase_group *bpg, const broad_phase_environement_query_results &bpeqr);

int    bp_env_jq_batch_function2(jqBatch *pBatch);
void    broad_phase_process();
void broad_phase_system_init(
                const broad_phase_memory_info *bpmi,
                bool (*should_collide_callback)(const broad_phase_base *, const broad_phase_base *));
void broad_phase_system_shutdown();

void    process_cluster_environment_collision_prolog(broad_phase_info *bpb, broad_phase_base *info);
bool compare_bpb(broad_phase_base *bpb1, broad_phase_base *bpb2);


bool phys_are_potentially_colliding_whace_broad_phase_info_broad_phase_info_(
                broad_phase_info *p1,
                broad_phase_info *p2,
                float *hit_time);
void add_collision_pair(
                broad_phase_info *bpi1,
                broad_phase_info *bpi2,
                float hit_time,
                phys_gjk_cache_info *gjk_ci);
void add_collision_pair_mutex(
                broad_phase_info *bpi1,
                broad_phase_info *bpi2,
                float hit_time,
                phys_gjk_cache_info *gjk_ci);


void    broad_phase_process_collision_pairs();
void __cdecl process_cluster_environment_collision(broad_phase_base *bpb, const broad_phase_environement_query_results &bpeqr);

bool phys_are_potentially_colliding(
    const phys_vec3 *aabb_min0,
    const phys_vec3 *aabb_max0,
    const phys_vec3 *aabb0_translation,
    const phys_vec3 *aabb_min1,
    const phys_vec3 *aabb_max1,
    float *hit_time);

extern broad_phase_memory *G_BPM;
