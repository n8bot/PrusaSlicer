#ifndef WipeTower_
#define WipeTower_

#include <cmath>
#include <string>
#include <sstream>
#include <utility>
#include <algorithm>

#include "libslic3r/Point.hpp"

namespace Slic3r
{

class WipeTowerWriter;
class PrintConfig;
enum GCodeFlavor : unsigned char;



class WipeTower
{
public:
    static const std::string never_skip_tag() { return "_GCODE_WIPE_TOWER_NEVER_SKIP_TAG"; }

    struct Extrusion
    {
		Extrusion(const Vec2f &pos, float width, unsigned int tool) : pos(pos), width(width), tool(tool) {}
		// End position of this extrusion.
		Vec2f				pos;
		// Width of a squished extrusion, corrected for the roundings of the squished extrusions.
		// This is left zero if it is a travel move.
		float 			width;
		// Current extruder index.
		unsigned int    tool;
	};

	struct ToolChangeResult
	{
		// Print heigh of this tool change.
		float					print_z;
		float 					layer_height;
		// G-code section to be directly included into the output G-code.
		std::string				gcode;
		// For path preview.
		std::vector<Extrusion> 	extrusions;
		// Initial position, at which the wipe tower starts its action.
		// At this position the extruder is loaded and there is no Z-hop applied.
		Vec2f						start_pos;
		// Last point, at which the normal G-code generator of Slic3r shall continue.
		// At this position the extruder is loaded and there is no Z-hop applied.
		Vec2f						end_pos;
		// Time elapsed over this tool change.
		// This is useful not only for the print time estimation, but also for the control of layer cooling.
		float  				    elapsed_time;

        // Is this a priming extrusion? (If so, the wipe tower rotation & translation will not be applied later)
        bool                    priming;

        // Pass a polyline so that normal G-code generator can do a wipe for us.
        // The wipe cannot be done by the wipe tower because it has to pass back
        // a loaded extruder, so it would have to either do a wipe with no retraction
        // (leading to https://github.com/prusa3d/PrusaSlicer/issues/2834) or do
        // an extra retraction-unretraction pair.
        std::vector<Vec2f> wipe_path;

        // Initial tool
        int initial_tool;

        // New tool
        int new_tool;

		// Sum the total length of the extrusion.
		float total_extrusion_length_in_plane() {
			float e_length = 0.f;
			for (size_t i = 1; i < this->extrusions.size(); ++ i) {
				const Extrusion &e = this->extrusions[i];
				if (e.width > 0) {
					Vec2f v = e.pos - (&e - 1)->pos;
					e_length += v.norm();
				}
			}
			return e_length;
		}
	};

    // Construct ToolChangeResult from current state of WipeTower and WipeTowerWriter.
    // WipeTowerWriter is moved from !
    ToolChangeResult construct_tcr(WipeTowerWriter& writer,
                                   bool priming,
                                   size_t old_tool) const;

	// x			-- x coordinates of wipe tower in mm ( left bottom corner )
	// y			-- y coordinates of wipe tower in mm ( left bottom corner )
	// width		-- width of wipe tower in mm ( default 60 mm - leave as it is )
	// wipe_area	-- space available for one toolchange in mm
    WipeTower(const PrintConfig& config, const std::vector<std::vector<float>>& wiping_matrix, size_t initial_tool);


	// Set the extruder properties.
    void set_extruder(size_t idx, const PrintConfig& config);

	// Appends into internal structure m_plan containing info about the future wipe tower
	// to be used before building begins. The entries must be added ordered in z.
	void plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool, unsigned int new_tool, bool brim, float wipe_volume = 0.f);

	// Iterates through prepared m_plan, generates ToolChangeResults and appends them to "result"
	void generate(std::vector<std::vector<ToolChangeResult>> &result);

    float get_depth() const { return m_wipe_tower_depth; }
    float get_brim_width() const { return m_wipe_tower_brim_width; }





	// Switch to a next layer.
	void set_layer(
		// Print height of this layer.
		float print_z,
		// Layer height, used to calculate extrusion the rate.
		float layer_height,
		// Maximum number of tool changes on this layer or the layers below.
		size_t max_tool_changes,
		// Is this the first layer of the print? In that case print the brim first.
		bool is_first_layer,
		// Is this the last layer of the waste tower?
		bool is_last_layer)
	{
		m_z_pos 				= print_z;
		m_layer_height			= layer_height;
		m_is_first_layer 		= is_first_layer;
		m_print_brim = is_first_layer;
		m_depth_traversed  = 0.f;
		m_current_shape = (! is_first_layer && m_current_shape == SHAPE_NORMAL) ? SHAPE_REVERSED : SHAPE_NORMAL;
		if (is_first_layer) {
            this->m_num_layer_changes 	= 0;
            this->m_num_tool_changes 	= 0;
        }
        else
            ++ m_num_layer_changes;
		
		// Calculate extrusion flow from desired line width, nozzle diameter, filament diameter and layer_height:
		m_extrusion_flow = extrusion_flow(layer_height);

        // Advance m_layer_info iterator, making sure we got it right
		while (!m_plan.empty() && m_layer_info->z < print_z - WT_EPSILON && m_layer_info+1 != m_plan.end())
			++m_layer_info;
	}

	// Return the wipe tower position.
	const Vec2f& 		 position() const { return m_wipe_tower_pos; }
	// Return the wipe tower width.
	float     		 width()    const { return m_wipe_tower_width; }
	// The wipe tower is finished, there should be no more tool changes or wipe tower prints.
	bool 	  		 finished() const { return m_max_color_changes == 0; }

	// Returns gcode to prime the nozzles at the front edge of the print bed.
	std::vector<ToolChangeResult> prime(
		// print_z of the first layer.
		float 						first_layer_height, 
		// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
		const std::vector<unsigned int> &tools,
		// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
		// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
		bool 						last_wipe_inside_wipe_tower);

	// Returns gcode for a toolchange and a final print head position.
	// On the first layer, extrude a brim around the future wipe tower first.
    ToolChangeResult tool_change(size_t new_tool);

	// Fill the unfilled space with a sparse infill.
	// Call this method only if layer_finished() is false.
	ToolChangeResult finish_layer();

	// Is the current layer finished?
	bool 			 layer_finished() const {
		return ( (m_is_first_layer ? m_wipe_tower_depth - m_perimeter_width : m_layer_info->depth) - WT_EPSILON < m_depth_traversed);
	}

    std::vector<float> get_used_filament() const { return m_used_filament_length; }
    int get_number_of_toolchanges() const { return m_num_tool_changes; }

    struct FilamentParameters {
        std::string 	    material = "PLA";
        int  			    temperature = 0;
        int  			    first_layer_temperature = 0;
        float               loading_speed = 0.f;
        float               loading_speed_start = 0.f;
        float               unloading_speed = 0.f;
        float               unloading_speed_start = 0.f;
        float               delay = 0.f ;
        int                 cooling_moves = 0;
        float               cooling_initial_speed = 0.f;
        float               cooling_final_speed = 0.f;
        float               ramming_line_width_multiplicator = 1.f;
        float               ramming_step_multiplicator = 1.f;
        float               max_e_speed = std::numeric_limits<float>::max();
        std::vector<float>  ramming_speed;
        float               nozzle_diameter;
        float               filament_area;
    };

private:
	enum wipe_shape // A fill-in direction
	{
		SHAPE_NORMAL = 1,
		SHAPE_REVERSED = -1
	};

    const bool  m_peters_wipe_tower   = false; // sparse wipe tower inspired by Peter's post processor - not finished yet
    const float Width_To_Nozzle_Ratio = 1.25f; // desired line width (oval) in multiples of nozzle diameter - may not be actually neccessary to adjust
    const float WT_EPSILON            = 1e-3f;
    float filament_area() const {
        return m_filpar[0].filament_area; // all extruders are assumed to have the same filament diameter at this point
    }


	bool   m_semm               = true; // Are we using a single extruder multimaterial printer?
    Vec2f  m_wipe_tower_pos; 			// Left front corner of the wipe tower in mm.
	float  m_wipe_tower_width; 			// Width of the wipe tower.
	float  m_wipe_tower_depth 	= 0.f; 	// Depth of the wipe tower
    float  m_wipe_tower_brim_width     = 0.f; 	// Width of brim (mm)
	float  m_wipe_tower_rotation_angle = 0.f; // Wipe tower rotation angle in degrees (with respect to x axis)
    float  m_internal_rotation  = 0.f;
	float  m_y_shift			= 0.f;  // y shift passed to writer
	float  m_z_pos 				= 0.f;  // Current Z position.
	float  m_layer_height 		= 0.f; 	// Current layer height.
	size_t m_max_color_changes 	= 0; 	// Maximum number of color changes per layer.
	bool   m_is_first_layer 	= false;// Is this the 1st layer of the print? If so, print the brim around the waste tower.
    int    m_old_temperature    = -1;   // To keep track of what was the last temp that we set (so we don't issue the command when not neccessary)

	// G-code generator parameters.
    float           m_cooling_tube_retraction   = 0.f;
    float           m_cooling_tube_length       = 0.f;
    float           m_parking_pos_retraction    = 0.f;
    float           m_extra_loading_move        = 0.f;
    float           m_bridging                  = 0.f;
    bool            m_no_sparse_layers          = false;
    bool            m_set_extruder_trimpot      = false;
    bool            m_adhesion                  = true;
    GCodeFlavor     m_gcode_flavor;

    // Bed properties
    enum {
        RectangularBed,
        CircularBed
    } m_bed_shape;
    float m_bed_width; // width of the bed bounding box
    Vec2f m_bed_bottom_left; // bottom-left corner coordinates (for rectangular beds)

	float m_perimeter_width = 0.4f * Width_To_Nozzle_Ratio; // Width of an extrusion line, also a perimeter spacing for 100% infill.
	float m_extrusion_flow = 0.038f; //0.029f;// Extrusion flow is derived from m_perimeter_width, layer height and filament diameter.

	// Extruder specific parameters.
    std::vector<FilamentParameters> m_filpar;


	// State of the wipe tower generator.
	unsigned int m_num_layer_changes = 0; // Layer change counter for the output statistics.
	unsigned int m_num_tool_changes  = 0; // Tool change change counter for the output statistics.
	///unsigned int 	m_idx_tool_change_in_layer = 0; // Layer change counter in this layer. Counting up to m_max_color_changes.
	bool m_print_brim = true;
	// A fill-in direction (positive Y, negative Y) alternates with each layer.
	wipe_shape   	m_current_shape = SHAPE_NORMAL;
    size_t 	m_current_tool  = 0;
    const std::vector<std::vector<float>> wipe_volumes;

	float           m_depth_traversed = 0.f; // Current y position at the wipe tower.
	bool 			m_left_to_right   = true;
	float			m_extra_spacing   = 1.f;

	// Calculates extrusion flow needed to produce required line width for given layer height
	float extrusion_flow(float layer_height = -1.f) const	// negative layer_height - return current m_extrusion_flow
	{
		if ( layer_height < 0 )
			return m_extrusion_flow;
		return layer_height * m_perimeter_width / filament_area();
	}

	// Calculates length of extrusion line to extrude given volume
	float volume_to_length(float volume, float line_width, float layer_height) const {
		return std::max(0.f, volume / (layer_height * line_width));
	}

	// Calculates depth for all layers and propagates them downwards
	void plan_tower();

	// Goes through m_plan and recalculates depths and width of the WT to make it exactly square - experimental
	void make_wipe_tower_square();

    // Goes through m_plan, calculates border and finish_layer extrusions and subtracts them from last wipe
    void save_on_last_wipe();


	struct box_coordinates
	{
		box_coordinates(float left, float bottom, float width, float height) :
			ld(left        , bottom         ),
			lu(left        , bottom + height),
			rd(left + width, bottom         ),
			ru(left + width, bottom + height) {}
		box_coordinates(const Vec2f &pos, float width, float height) : box_coordinates(pos(0), pos(1), width, height) {}
		void translate(const Vec2f &shift) {
			ld += shift; lu += shift;
			rd += shift; ru += shift;
		}
		void translate(const float dx, const float dy) { translate(Vec2f(dx, dy)); }
		void expand(const float offset) {
			ld += Vec2f(- offset, - offset);
			lu += Vec2f(- offset,   offset);
			rd += Vec2f(  offset, - offset);
			ru += Vec2f(  offset,   offset);
		}
		void expand(const float offset_x, const float offset_y) {
			ld += Vec2f(- offset_x, - offset_y);
			lu += Vec2f(- offset_x,   offset_y);
			rd += Vec2f(  offset_x, - offset_y);
			ru += Vec2f(  offset_x,   offset_y);
		}
		Vec2f ld;  // left down
		Vec2f lu;	// left upper 
		Vec2f rd;	// right lower
		Vec2f ru;  // right upper
	};


	// to store information about tool changes for a given layer
	struct WipeTowerInfo{
		struct ToolChange {
            size_t old_tool;
            size_t new_tool;
			float required_depth;
            float ramming_depth;
            float first_wipe_line;
            float wipe_volume;
            ToolChange(size_t old, size_t newtool, float depth=0.f, float ramming_depth=0.f, float fwl=0.f, float wv=0.f)
            : old_tool{old}, new_tool{newtool}, required_depth{depth}, ramming_depth{ramming_depth}, first_wipe_line{fwl}, wipe_volume{wv} {}
		};
		float z;		// z position of the layer
		float height;	// layer height
		float depth;	// depth of the layer based on all layers above
		float extra_spacing;
		float toolchanges_depth() const { float sum = 0.f; for (const auto &a : tool_changes) sum += a.required_depth; return sum; }

		std::vector<ToolChange> tool_changes;

		WipeTowerInfo(float z_par, float layer_height_par)
			: z{z_par}, height{layer_height_par}, depth{0}, extra_spacing{1.f} {}
	};

	std::vector<WipeTowerInfo> m_plan; 	// Stores information about all layers and toolchanges for the future wipe tower (filled by plan_toolchange(...))
	std::vector<WipeTowerInfo>::iterator m_layer_info = m_plan.end();

    // Stores information about used filament length per extruder:
    std::vector<float> m_used_filament_length;


	// Returns gcode for wipe tower brim
	// sideOnly			-- set to false -- experimental, draw brim on sides of wipe tower
	// offset			-- set to 0		-- experimental, offset to replace brim in front / rear of wipe tower
	ToolChangeResult toolchange_Brim(bool sideOnly = false, float y_offset = 0.f);

	void toolchange_Unload(
		WipeTowerWriter &writer,
		const box_coordinates  &cleaning_box, 
		const std::string&	 	current_material,
		const int 				new_temperature);

	void toolchange_Change(
		WipeTowerWriter &writer,
        const size_t		new_tool,
		const std::string& 		new_material);
	
	void toolchange_Load(
		WipeTowerWriter &writer,
		const box_coordinates  &cleaning_box);
	
	void toolchange_Wipe(
		WipeTowerWriter &writer,
		const box_coordinates  &cleaning_box,
		float wipe_volume);
};




}; // namespace Slic3r

#endif // WipeTowerPrusaMM_hpp_ 
