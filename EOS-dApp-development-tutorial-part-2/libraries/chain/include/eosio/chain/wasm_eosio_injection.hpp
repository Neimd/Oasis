#pragma once

#include <eosio/chain/wasm_eosio_binary_ops.hpp>
#include <eosio/chain/wasm_eosio_constraints.hpp>
#include <eosio/chain/webassembly/common.hpp>
#include <fc/exception/exception.hpp>
#include <eosio/chain/exceptions.hpp>
#include <iostream>
#include <functional>
#include <vector>
#include <map>
#include <stack>
#include <queue>
#include <unordered_set>
#include "IR/Module.h"
#include "IR/Operators.h"
#include "WASM/WASM.h"


namespace eosio { namespace chain { namespace wasm_injections {
   using namespace IR;
   // helper functions for injection

   struct injector_utils {
      static std::map<std::vector<uint16_t>, uint32_t> type_slots;
      static std::map<std::string, uint32_t>           registered_injected;
      static std::map<uint32_t, uint32_t>              injected_index_mapping;
      static uint32_t                                  next_injected_index;

      static void init( Module& mod ) { 
         type_slots.clear(); 
         registered_injected.clear();
         injected_index_mapping.clear();
         build_type_slots( mod );
         next_injected_index = 0;
      }

      static void build_type_slots( Module& mod ) {
         // add the module types to the type_slots map
         for ( int i=0; i < mod.types.size(); i++ ) {
            std::vector<uint16_t> type_slot_list = { static_cast<uint16_t>(mod.types[i]->ret) };
            for ( auto param : mod.types[i]->parameters )
               type_slot_list.push_back( static_cast<uint16_t>(param) );
            type_slots.emplace( type_slot_list, i );
         } 
      }

      template <ResultType Result, ValueType... Params>
      static void add_type_slot( Module& mod ) {
         if ( type_slots.find({FromResultType<Result>::value, FromValueType<Params>::value...}) == type_slots.end() ) {
            type_slots.emplace( std::vector<uint16_t>{FromResultType<Result>::value, FromValueType<Params>::value...}, mod.types.size() );
            mod.types.push_back( FunctionType::get( Result, { Params... } ) );
         }
      }

      // get the next available index that is greater than the last exported function
      static void get_next_indices( Module& module, int& next_function_index, int& next_actual_index ) {
         int exports = 0;
         for ( auto exp : module.exports )
            if ( exp.kind == IR::ObjectKind::function )
               exports++;

         next_function_index = module.functions.imports.size() + module.functions.defs.size() + registered_injected.size();
         next_actual_index = next_injected_index++;
      }

      template <ResultType Result, ValueType... Params>
      static void add_import(Module& module, const char* func_name, int32_t& index ) {
         if (module.functions.imports.size() == 0 || registered_injected.find(func_name) == registered_injected.end() ) {
            add_type_slot<Result, Params...>( module );
            const uint32_t func_type_index = type_slots[{ FromResultType<Result>::value, FromValueType<Params>::value... }];
            int actual_index;
            get_next_indices( module, index, actual_index );
            registered_injected.emplace( func_name, index );
            decltype(module.functions.imports) new_import = { {{func_type_index}, EOSIO_INJECTED_MODULE_NAME, std::move(func_name)} };
            // prepend to the head of the imports
            module.functions.imports.insert( module.functions.imports.begin()+(registered_injected.size()-1), new_import.begin(), new_import.end() ); 
            injected_index_mapping.emplace( index, actual_index ); 

            // shift all exported functions by 1
            for ( int i=0; i < module.exports.size(); i++ ) {
               if ( module.exports[i].kind == IR::ObjectKind::function ) {
                  module.exports[i].index++;
               }
            }

            // update the start function
            if ( module.startFunctionIndex != UINTPTR_MAX ) {
               module.startFunctionIndex++;
            }

            // shift all table entries for call indirect
            for(TableSegment& ts : module.tableSegments) {
               for(auto& idx : ts.indices)
                  idx++;
            }
         }
         else {
            index = registered_injected[func_name];
         }
      }
   };
   
   struct noop_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   struct memories_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   struct data_segments_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };
   
   struct tables_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   struct globals_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   struct max_memory_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   struct blacklist_injection_visitor {
      static void inject( IR::Module& m );
      static void initializer();
   };

   using wasm_validate_func = std::function<void(IR::Module&)>;

  
   // just pass 
   struct no_injections_injectors {
      static void inject( IR::Module& m ) {}
   };

   // simple mutator that doesn't actually mutate anything
   // used to verify that a given instruction is valid for execution on our platform
   // injector 'interface' :
   //          `kills` -> should this injector kill the original instruction
   //          `post`  -> should this injector inject after the original instruction
   struct pass_injector {
      static constexpr bool kills = false;
      static constexpr bool post = false;
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
      }
   };

   struct instruction_counter {
      static constexpr bool kills = false;
      static constexpr bool post = false;
      static void init() { 
         icnt=0; 
         tcnt=0; 
         bcnt=0; 
         while ( !fcnts.empty() )
            fcnts.pop();
      }
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         icnt++;
         tcnt++;
      }
      static uint32_t icnt; /* instructions so far */
      static uint32_t tcnt; /* total instructions */
      static uint32_t bcnt; /* total instructions from block types */
      static std::queue<uint32_t> fcnts; 
   };

   struct checktime_block_type {
      static constexpr bool kills = false;
      static constexpr bool post = false;
#define CLEAR(x) \
      while( !x.empty() ) \
         x.pop()

      static void init() { 
         CLEAR(block_stack);
         CLEAR(type_stack);
         CLEAR(orderings);
         CLEAR(bcnt_tables);
      }
#undef CLEAR
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         instruction_counter::icnt = 0;
         block_stack.push(arg.start_index);
         orderings.back().push_back(arg.start_index);
         bcnt_tables.back().emplace(arg.start_index, 0);
         type_stack.push(inst->get_code() == wasm_ops::loop_code);
      }

      static std::stack<size_t>                   block_stack;
      static std::stack<size_t>                   type_stack; /* this might capture more than if a block is a loop in the future */
      static std::queue<std::vector<size_t>>      orderings;  /* record the order in which we found the blocks */
      static std::queue<std::map<size_t, size_t>> bcnt_tables; /* table for each blocks instruction count */
   };

   struct checktime_end {
      static constexpr bool kills = false;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         if ( checktime_block_type::type_stack.empty() ) 
            return;
         if ( !checktime_block_type::type_stack.top() ) { // empty or is not a loop
            checktime_block_type::block_stack.pop();
            checktime_block_type::type_stack.pop();
            return;
         }
         size_t inst_idx = checktime_block_type::block_stack.top();
         checktime_block_type::bcnt_tables.back()[inst_idx] = instruction_counter::icnt;
         instruction_counter::bcnt += instruction_counter::icnt;
         instruction_counter::icnt = 0;
         checktime_block_type::block_stack.pop();
         checktime_block_type::type_stack.pop();
      }
   };

   struct checktime_function_end {
      static constexpr bool kills = false;
      static constexpr bool post = false;
      static void init() { fcnt = 0; }
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         size_t inst_idx = checktime_block_type::block_stack.top();
         fcnt = instruction_counter::tcnt - instruction_counter::bcnt;
      }
      static size_t fcnt;
   };

   struct checktime_injection {
      static constexpr bool kills = false;
      static constexpr bool post = true;
      static void init() {
         idx = 0;
         chktm_idx = 0;
      }
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         auto mapped_index = injector_utils::injected_index_mapping.find(chktm_idx);

         wasm_ops::op_types<>::i32_const_t cnt; 
         cnt.field = checktime_block_type::bcnt_tables.front()[checktime_block_type::orderings.front()[idx++]];
         wasm_ops::op_types<>::call_t chktm; 
         chktm.field = mapped_index->second;
         cnt.pack(arg.new_code);
         chktm.pack(arg.new_code);
      }

      static int32_t idx;
      static int32_t chktm_idx;
   };

   struct fix_call_index {
      static constexpr bool kills = false;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         wasm_ops::op_types<>::call_t* call_inst = reinterpret_cast<wasm_ops::op_types<>::call_t*>(inst);
         auto mapped_index = injector_utils::injected_index_mapping.find(call_inst->field);

         if ( mapped_index != injector_utils::injected_index_mapping.end() )  {
            call_inst->field = mapped_index->second;
         }
         else {
            call_inst->field += injector_utils::registered_injected.size();
         }
      }

   };
   
   struct call_depth_check {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static int32_t global_idx; 
      static void init() {
         global_idx = -1;
      }
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         if ( global_idx == -1 ) {
            arg.module->globals.defs.push_back({{ValueType::i32, true}, {(I32) eosio::chain::wasm_constraints::maximum_call_depth}});
         }

         global_idx = arg.module->globals.size()-1;

         int32_t assert_idx;
         injector_utils::add_import<ResultType::none>(*(arg.module), "call_depth_assert", assert_idx);

         wasm_ops::op_types<>::call_t call_assert;
         wasm_ops::op_types<>::get_global_t get_global_inst; 
         wasm_ops::op_types<>::set_global_t set_global_inst;

         wasm_ops::op_types<>::i32_eqz_t eqz_inst; 
         wasm_ops::op_types<>::i32_const_t const_inst; 
         wasm_ops::op_types<>::i32_add_t add_inst;
         wasm_ops::op_types<>::end_t end_inst;
         wasm_ops::op_types<>::if__t if_inst; 
         wasm_ops::op_types<>::else__t else_inst; 

         call_assert.field = assert_idx;
         get_global_inst.field = global_idx;
         set_global_inst.field = global_idx;
         const_inst.field = -1;

#define INSERT_INJECTED(X)       \
         X.pack(arg.new_code);   \

         INSERT_INJECTED(get_global_inst);
         INSERT_INJECTED(eqz_inst);
         INSERT_INJECTED(if_inst);
         INSERT_INJECTED(call_assert);
         INSERT_INJECTED(else_inst);
         INSERT_INJECTED(const_inst);
         INSERT_INJECTED(get_global_inst);
         INSERT_INJECTED(add_inst);
         INSERT_INJECTED(set_global_inst);
         INSERT_INJECTED(end_inst);

         /* print the correct call type */
         if ( inst->get_code() == wasm_ops::call_code ) {
            wasm_ops::op_types<>::call_t* call_inst = reinterpret_cast<wasm_ops::op_types<>::call_t*>(inst);
            call_inst->pack(arg.new_code);
         }
         else {
            wasm_ops::op_types<>::call_indirect_t* call_inst = reinterpret_cast<wasm_ops::op_types<>::call_indirect_t*>(inst);
            call_inst->pack(arg.new_code);
         }

         const_inst.field = 1;
         INSERT_INJECTED(get_global_inst); 
         INSERT_INJECTED(const_inst);
         INSERT_INJECTED(add_inst);
         INSERT_INJECTED(set_global_inst);

#undef INSERT_INJECTED
      }
   }; 

   // float injections
   constexpr const char* inject_which_op( uint16_t opcode ) {
      switch ( opcode ) {
         case wasm_ops::f32_add_code:
            return u8"_eosio_f32_add";
         case wasm_ops::f32_sub_code:
            return u8"_eosio_f32_sub";
         case wasm_ops::f32_mul_code:
            return u8"_eosio_f32_mul";
         case wasm_ops::f32_div_code:
            return u8"_eosio_f32_div";
         case wasm_ops::f32_min_code:
            return u8"_eosio_f32_min";
         case wasm_ops::f32_max_code:
            return u8"_eosio_f32_max";
         case wasm_ops::f32_copysign_code:
            return u8"_eosio_f32_copysign";
         case wasm_ops::f32_abs_code:
            return u8"_eosio_f32_abs";
         case wasm_ops::f32_neg_code:
            return u8"_eosio_f32_neg";
         case wasm_ops::f32_sqrt_code:
            return u8"_eosio_f32_sqrt";
         case wasm_ops::f32_ceil_code:
            return u8"_eosio_f32_ceil";
         case wasm_ops::f32_floor_code:
            return u8"_eosio_f32_floor";
         case wasm_ops::f32_trunc_code:
            return u8"_eosio_f32_trunc";
         case wasm_ops::f32_nearest_code:
            return u8"_eosio_f32_nearest";
         case wasm_ops::f32_eq_code:
            return u8"_eosio_f32_eq";
         case wasm_ops::f32_ne_code:
            return u8"_eosio_f32_ne";
         case wasm_ops::f32_lt_code:
            return u8"_eosio_f32_lt";
         case wasm_ops::f32_le_code:
            return u8"_eosio_f32_le";
         case wasm_ops::f32_gt_code:
            return u8"_eosio_f32_gt";
         case wasm_ops::f32_ge_code:
            return u8"_eosio_f32_ge";
         case wasm_ops::f64_add_code:
            return u8"_eosio_f64_add";
         case wasm_ops::f64_sub_code:
            return u8"_eosio_f64_sub";
         case wasm_ops::f64_mul_code:
            return u8"_eosio_f64_mul";
         case wasm_ops::f64_div_code:
            return u8"_eosio_f64_div";
         case wasm_ops::f64_min_code:
            return u8"_eosio_f64_min";
         case wasm_ops::f64_max_code:
            return u8"_eosio_f64_max";
         case wasm_ops::f64_copysign_code:
            return u8"_eosio_f64_copysign";
         case wasm_ops::f64_abs_code:
            return u8"_eosio_f64_abs";
         case wasm_ops::f64_neg_code:
            return u8"_eosio_f64_neg";
         case wasm_ops::f64_sqrt_code:
            return u8"_eosio_f64_sqrt";
         case wasm_ops::f64_ceil_code:
            return u8"_eosio_f64_ceil";
         case wasm_ops::f64_floor_code:
            return u8"_eosio_f64_floor";
         case wasm_ops::f64_trunc_code:
            return u8"_eosio_f64_trunc";
         case wasm_ops::f64_nearest_code:
            return u8"_eosio_f64_nearest";
         case wasm_ops::f64_eq_code:
            return u8"_eosio_f64_eq";
         case wasm_ops::f64_ne_code:
            return u8"_eosio_f64_ne";
         case wasm_ops::f64_lt_code:
            return u8"_eosio_f64_lt";
         case wasm_ops::f64_le_code:
            return u8"_eosio_f64_le";
         case wasm_ops::f64_gt_code:
            return u8"_eosio_f64_gt";
         case wasm_ops::f64_ge_code:
            return u8"_eosio_f64_ge";
         case wasm_ops::f64_promote_f32_code:
            return u8"_eosio_f32_promote";
         case wasm_ops::f32_demote_f64_code:
            return u8"_eosio_f64_demote";
         case wasm_ops::i32_trunc_u_f32_code:
            return u8"_eosio_f32_trunc_i32u";
         case wasm_ops::i32_trunc_s_f32_code:
            return u8"_eosio_f32_trunc_i32s";
         case wasm_ops::i32_trunc_u_f64_code:
            return u8"_eosio_f64_trunc_i32u";
         case wasm_ops::i32_trunc_s_f64_code:
            return u8"_eosio_f64_trunc_i32s";
         case wasm_ops::i64_trunc_u_f32_code:
            return u8"_eosio_f32_trunc_i64u";
         case wasm_ops::i64_trunc_s_f32_code:
            return u8"_eosio_f32_trunc_i64s";
         case wasm_ops::i64_trunc_u_f64_code:
            return u8"_eosio_f64_trunc_i64u";
         case wasm_ops::i64_trunc_s_f64_code:
            return u8"_eosio_f64_trunc_i64s";
         case wasm_ops::f32_convert_s_i32_code:
            return u8"_eosio_i32_to_f32";
         case wasm_ops::f32_convert_u_i32_code:
            return u8"_eosio_ui32_to_f32";
         case wasm_ops::f32_convert_s_i64_code:
            return u8"_eosio_i64_f32";
         case wasm_ops::f32_convert_u_i64_code:
            return u8"_eosio_ui64_to_f32";
         case wasm_ops::f64_convert_s_i32_code:
            return u8"_eosio_i32_to_f64";
         case wasm_ops::f64_convert_u_i32_code:
            return u8"_eosio_ui32_to_f64";
         case wasm_ops::f64_convert_s_i64_code:
            return u8"_eosio_i64_to_f64";
         case wasm_ops::f64_convert_u_i64_code:
            return u8"_eosio_ui64_to_f64";

         default:
            FC_THROW_EXCEPTION( eosio::chain::wasm_execution_error, "Error, unknown opcode in injection ${op}", ("op", opcode));
      }
   }

   template <uint16_t Opcode>
   struct f32_binop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f32, ValueType::f32, ValueType::f32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };

   template <uint16_t Opcode>
   struct f32_unop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f32, ValueType::f32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };

   template <uint16_t Opcode>
   struct f32_relop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i32, ValueType::f32, ValueType::f32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint16_t Opcode>
   struct f64_binop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f64, ValueType::f64, ValueType::f64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f64op;
         f64op.field = idx;
         f64op.pack(arg.new_code);
      }
   };

   template <uint16_t Opcode>
   struct f64_unop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f64, ValueType::f64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f64op;
         f64op.field = idx;
         f64op.pack(arg.new_code);
      }
   };

   template <uint16_t Opcode>
   struct f64_relop_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i32, ValueType::f64, ValueType::f64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f64op;
         f64op.field = idx;
         f64op.pack(arg.new_code);
      }
   };

   template <uint16_t Opcode>
   struct f32_trunc_i32_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i32, ValueType::f32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint16_t Opcode>
   struct f32_trunc_i64_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i64, ValueType::f32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint16_t Opcode>
   struct f64_trunc_i32_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i32, ValueType::f64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint16_t Opcode>
   struct f64_trunc_i64_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::i64, ValueType::f64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint64_t Opcode>
   struct i32_convert_f32_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f32, ValueType::i32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint64_t Opcode>
   struct i64_convert_f32_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f32, ValueType::i64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f32op;
         f32op.field = idx;
         f32op.pack(arg.new_code);
      }
   };
   template <uint64_t Opcode>
   struct i32_convert_f64_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f64, ValueType::i32>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f64op;
         f64op.field = idx;
         f64op.pack(arg.new_code);
      }
   };
   template <uint64_t Opcode>
   struct i64_convert_f64_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f64, ValueType::i64>( *(arg.module), inject_which_op(Opcode), idx );
         wasm_ops::op_types<>::call_t f64op;
         f64op.field = idx;
         f64op.pack(arg.new_code);
      }
   };

   struct f32_promote_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f64, ValueType::f32>( *(arg.module), u8"_eosio_f32_promote", idx );
         wasm_ops::op_types<>::call_t f32promote;
         f32promote.field = idx;
         f32promote.pack(arg.new_code);
      }
   };
   
   struct f64_demote_injector {
      static constexpr bool kills = true;
      static constexpr bool post = false;
      static void init() {}
      static void accept( wasm_ops::instr* inst, wasm_ops::visitor_arg& arg ) {
         int32_t idx;
         injector_utils::add_import<ResultType::f32, ValueType::f64>( *(arg.module), u8"_eosio_f64_demote", idx );
         wasm_ops::op_types<>::call_t f32promote;
         f32promote.field = idx;
         f32promote.pack(arg.new_code);
      }
   };

   struct pre_op_injectors : wasm_ops::op_types<pass_injector> {
      using block_t           = wasm_ops::block                   <instruction_counter>;
      using loop_t            = wasm_ops::loop                    <instruction_counter, checktime_block_type>;
      using if__t             = wasm_ops::if_                     <instruction_counter>;
      using else__t           = wasm_ops::else_                   <instruction_counter>;
      
      using end_t             = wasm_ops::end                     <instruction_counter, checktime_end>;
      using unreachable_t     = wasm_ops::unreachable             <instruction_counter>;
      using br_t              = wasm_ops::br                      <instruction_counter>;
      using br_if_t           = wasm_ops::br_if                   <instruction_counter>;
      using br_table_t        = wasm_ops::br_table                <instruction_counter>;
      using return__t         = wasm_ops::return_                 <instruction_counter>;
      using call_t            = wasm_ops::call                    <instruction_counter, call_depth_check>;
      using call_indirect_t   = wasm_ops::call_indirect           <instruction_counter, call_depth_check>;
      using drop_t            = wasm_ops::drop                    <instruction_counter>;
      using select_t          = wasm_ops::select                  <instruction_counter>;

      using get_local_t       = wasm_ops::get_local               <instruction_counter>;
      using set_local_t       = wasm_ops::set_local               <instruction_counter>;
      using tee_local_t       = wasm_ops::tee_local               <instruction_counter>;
      using get_global_t      = wasm_ops::get_global              <instruction_counter>;
      using set_global_t      = wasm_ops::set_global              <instruction_counter>;

      using nop_t             = wasm_ops::nop                     <instruction_counter>;
      using i32_load_t        = wasm_ops::i32_load                <instruction_counter>;
      using i64_load_t        = wasm_ops::i64_load                <instruction_counter>;
      using f32_load_t        = wasm_ops::f32_load                <instruction_counter>;
      using f64_load_t        = wasm_ops::f64_load                <instruction_counter>;
      using i32_load8_s_t     = wasm_ops::i32_load8_s             <instruction_counter>;
      using i32_load8_u_t     = wasm_ops::i32_load8_u             <instruction_counter>;
      using i32_load16_s_t    = wasm_ops::i32_load16_s            <instruction_counter>;
      using i32_load16_u_t    = wasm_ops::i32_load16_u            <instruction_counter>;
      using i64_load8_s_t     = wasm_ops::i64_load8_s             <instruction_counter>;
      using i64_load8_u_t     = wasm_ops::i64_load8_u             <instruction_counter>;
      using i64_load16_s_t    = wasm_ops::i64_load16_s            <instruction_counter>;
      using i64_load16_u_t    = wasm_ops::i64_load16_u            <instruction_counter>;
      using i64_load32_s_t    = wasm_ops::i64_load32_s            <instruction_counter>;
      using i64_load32_u_t    = wasm_ops::i64_load32_u            <instruction_counter>;
      using i32_store_t       = wasm_ops::i32_store               <instruction_counter>;
      using i64_store_t       = wasm_ops::i64_store               <instruction_counter>;
      using f32_store_t       = wasm_ops::f32_store               <instruction_counter>;
      using f64_store_t       = wasm_ops::f64_store               <instruction_counter>;
      using i32_store8_t      = wasm_ops::i32_store8              <instruction_counter>;
      using i32_store16_t     = wasm_ops::i32_store16             <instruction_counter>;
      using i64_store8_t      = wasm_ops::i64_store8              <instruction_counter>;
      using i64_store16_t     = wasm_ops::i64_store16             <instruction_counter>;
      using i64_store32_t     = wasm_ops::i64_store32             <instruction_counter>;

      using i32_const_t       = wasm_ops::i32_const               <instruction_counter>;
      using i64_const_t       = wasm_ops::i64_const               <instruction_counter>;
      using f32_const_t       = wasm_ops::f32_const               <instruction_counter>;
      using f64_const_t       = wasm_ops::f64_const               <instruction_counter>;
      
      using i32_eqz_t         = wasm_ops::i32_eqz                 <instruction_counter>;
      using i32_eq_t          = wasm_ops::i32_eq                  <instruction_counter>;
      using i32_ne_t          = wasm_ops::i32_ne                  <instruction_counter>;
      using i32_lt_s_t        = wasm_ops::i32_lt_s                <instruction_counter>;
      using i32_lt_u_t        = wasm_ops::i32_lt_u                <instruction_counter>;
      using i32_gt_s_t        = wasm_ops::i32_gt_s                <instruction_counter>;
      using i32_gt_u_t        = wasm_ops::i32_gt_u                <instruction_counter>;
      using i32_le_s_t        = wasm_ops::i32_le_s                <instruction_counter>;
      using i32_le_u_t        = wasm_ops::i32_le_u                <instruction_counter>;
      using i32_ge_s_t        = wasm_ops::i32_ge_s                <instruction_counter>;
      using i32_ge_u_t        = wasm_ops::i32_ge_u                <instruction_counter>;

      using i32_clz_t         = wasm_ops::i32_clz                 <instruction_counter>;
      using i32_ctz_t         = wasm_ops::i32_ctz                 <instruction_counter>;
      using i32_popcnt_t      = wasm_ops::i32_popcnt              <instruction_counter>;

      using i32_add_t         = wasm_ops::i32_add                 <instruction_counter>; 
      using i32_sub_t         = wasm_ops::i32_sub                 <instruction_counter>; 
      using i32_mul_t         = wasm_ops::i32_mul                 <instruction_counter>; 
      using i32_div_s_t       = wasm_ops::i32_div_s               <instruction_counter>; 
      using i32_div_u_t       = wasm_ops::i32_div_u               <instruction_counter>; 
      using i32_rem_s_t       = wasm_ops::i32_rem_s               <instruction_counter>; 
      using i32_rem_u_t       = wasm_ops::i32_rem_u               <instruction_counter>; 
      using i32_and_t         = wasm_ops::i32_and                 <instruction_counter>; 
      using i32_or_t          = wasm_ops::i32_or                  <instruction_counter>; 
      using i32_xor_t         = wasm_ops::i32_xor                 <instruction_counter>; 
      using i32_shl_t         = wasm_ops::i32_shl                 <instruction_counter>; 
      using i32_shr_s_t       = wasm_ops::i32_shr_s               <instruction_counter>; 
      using i32_shr_u_t       = wasm_ops::i32_shr_u               <instruction_counter>; 
      using i32_rotl_t        = wasm_ops::i32_rotl                <instruction_counter>; 
      using i32_rotr_t        = wasm_ops::i32_rotr                <instruction_counter>; 

      using i64_eqz_t         = wasm_ops::i64_eqz                 <instruction_counter>;
      using i64_eq_t          = wasm_ops::i64_eq                  <instruction_counter>;
      using i64_ne_t          = wasm_ops::i64_ne                  <instruction_counter>;
      using i64_lt_s_t        = wasm_ops::i64_lt_s                <instruction_counter>;
      using i64_lt_u_t        = wasm_ops::i64_lt_u                <instruction_counter>;
      using i64_gt_s_t        = wasm_ops::i64_gt_s                <instruction_counter>;
      using i64_gt_u_t        = wasm_ops::i64_gt_u                <instruction_counter>;
      using i64_le_s_t        = wasm_ops::i64_le_s                <instruction_counter>;
      using i64_le_u_t        = wasm_ops::i64_le_u                <instruction_counter>;
      using i64_ge_s_t        = wasm_ops::i64_ge_s                <instruction_counter>;
      using i64_ge_u_t        = wasm_ops::i64_ge_u                <instruction_counter>;

      using i64_clz_t         = wasm_ops::i64_clz                 <instruction_counter>;
      using i64_ctz_t         = wasm_ops::i64_ctz                 <instruction_counter>;
      using i64_popcnt_t      = wasm_ops::i64_popcnt              <instruction_counter>;

      using i64_add_t         = wasm_ops::i64_add                 <instruction_counter>; 
      using i64_sub_t         = wasm_ops::i64_sub                 <instruction_counter>; 
      using i64_mul_t         = wasm_ops::i64_mul                 <instruction_counter>; 
      using i64_div_s_t       = wasm_ops::i64_div_s               <instruction_counter>; 
      using i64_div_u_t       = wasm_ops::i64_div_u               <instruction_counter>; 
      using i64_rem_s_t       = wasm_ops::i64_rem_s               <instruction_counter>; 
      using i64_rem_u_t       = wasm_ops::i64_rem_u               <instruction_counter>; 
      using i64_and_t         = wasm_ops::i64_and                 <instruction_counter>; 
      using i64_or_t          = wasm_ops::i64_or                  <instruction_counter>; 
      using i64_xor_t         = wasm_ops::i64_xor                 <instruction_counter>; 
      using i64_shl_t         = wasm_ops::i64_shl                 <instruction_counter>; 
      using i64_shr_s_t       = wasm_ops::i64_shr_s               <instruction_counter>; 
      using i64_shr_u_t       = wasm_ops::i64_shr_u               <instruction_counter>; 
      using i64_rotl_t        = wasm_ops::i64_rotl                <instruction_counter>; 
      using i64_rotr_t        = wasm_ops::i64_rotr                <instruction_counter>; 

      // float binops 
      using f32_add_t         = wasm_ops::f32_add                 <instruction_counter, f32_binop_injector<wasm_ops::f32_add_code>>;
      using f32_sub_t         = wasm_ops::f32_sub                 <instruction_counter, f32_binop_injector<wasm_ops::f32_sub_code>>;
      using f32_div_t         = wasm_ops::f32_div                 <instruction_counter, f32_binop_injector<wasm_ops::f32_div_code>>;
      using f32_mul_t         = wasm_ops::f32_mul                 <instruction_counter, f32_binop_injector<wasm_ops::f32_mul_code>>;
      using f32_min_t         = wasm_ops::f32_min                 <instruction_counter, f32_binop_injector<wasm_ops::f32_min_code>>;
      using f32_max_t         = wasm_ops::f32_max                 <instruction_counter, f32_binop_injector<wasm_ops::f32_max_code>>;
      using f32_copysign_t    = wasm_ops::f32_copysign            <instruction_counter, f32_binop_injector<wasm_ops::f32_copysign_code>>;
      // float unops
      using f32_abs_t         = wasm_ops::f32_abs                 <instruction_counter, f32_unop_injector<wasm_ops::f32_abs_code>>;
      using f32_neg_t         = wasm_ops::f32_neg                 <instruction_counter, f32_unop_injector<wasm_ops::f32_neg_code>>;
      using f32_sqrt_t        = wasm_ops::f32_sqrt                <instruction_counter, f32_unop_injector<wasm_ops::f32_sqrt_code>>;
      using f32_floor_t       = wasm_ops::f32_floor               <instruction_counter, f32_unop_injector<wasm_ops::f32_floor_code>>;
      using f32_ceil_t        = wasm_ops::f32_ceil                <instruction_counter, f32_unop_injector<wasm_ops::f32_ceil_code>>;
      using f32_trunc_t       = wasm_ops::f32_trunc               <instruction_counter, f32_unop_injector<wasm_ops::f32_trunc_code>>;
      using f32_nearest_t     = wasm_ops::f32_nearest             <instruction_counter, f32_unop_injector<wasm_ops::f32_nearest_code>>;
      // float relops
      using f32_eq_t          = wasm_ops::f32_eq                  <instruction_counter, f32_relop_injector<wasm_ops::f32_eq_code>>;
      using f32_ne_t          = wasm_ops::f32_ne                  <instruction_counter, f32_relop_injector<wasm_ops::f32_ne_code>>;
      using f32_lt_t          = wasm_ops::f32_lt                  <instruction_counter, f32_relop_injector<wasm_ops::f32_lt_code>>;
      using f32_le_t          = wasm_ops::f32_le                  <instruction_counter, f32_relop_injector<wasm_ops::f32_le_code>>;
      using f32_gt_t          = wasm_ops::f32_gt                  <instruction_counter, f32_relop_injector<wasm_ops::f32_gt_code>>;
      using f32_ge_t          = wasm_ops::f32_ge                  <instruction_counter, f32_relop_injector<wasm_ops::f32_ge_code>>;

      // float binops 
      using f64_add_t         = wasm_ops::f64_add                 <instruction_counter, f64_binop_injector<wasm_ops::f64_add_code>>;
      using f64_sub_t         = wasm_ops::f64_sub                 <instruction_counter, f64_binop_injector<wasm_ops::f64_sub_code>>;
      using f64_div_t         = wasm_ops::f64_div                 <instruction_counter, f64_binop_injector<wasm_ops::f64_div_code>>;
      using f64_mul_t         = wasm_ops::f64_mul                 <instruction_counter, f64_binop_injector<wasm_ops::f64_mul_code>>;
      using f64_min_t         = wasm_ops::f64_min                 <instruction_counter, f64_binop_injector<wasm_ops::f64_min_code>>;
      using f64_max_t         = wasm_ops::f64_max                 <instruction_counter, f64_binop_injector<wasm_ops::f64_max_code>>;
      using f64_copysign_t    = wasm_ops::f64_copysign            <instruction_counter, f64_binop_injector<wasm_ops::f64_copysign_code>>;
      // float unops
      using f64_abs_t         = wasm_ops::f64_abs                 <instruction_counter, f64_unop_injector<wasm_ops::f64_abs_code>>;
      using f64_neg_t         = wasm_ops::f64_neg                 <instruction_counter, f64_unop_injector<wasm_ops::f64_neg_code>>;
      using f64_sqrt_t        = wasm_ops::f64_sqrt                <instruction_counter, f64_unop_injector<wasm_ops::f64_sqrt_code>>;
      using f64_floor_t       = wasm_ops::f64_floor               <instruction_counter, f64_unop_injector<wasm_ops::f64_floor_code>>;
      using f64_ceil_t        = wasm_ops::f64_ceil                <instruction_counter, f64_unop_injector<wasm_ops::f64_ceil_code>>;
      using f64_trunc_t       = wasm_ops::f64_trunc               <instruction_counter, f64_unop_injector<wasm_ops::f64_trunc_code>>;
      using f64_nearest_t     = wasm_ops::f64_nearest             <instruction_counter, f64_unop_injector<wasm_ops::f64_nearest_code>>;
      // float relops
      using f64_eq_t          = wasm_ops::f64_eq                  <instruction_counter, f64_relop_injector<wasm_ops::f64_eq_code>>;
      using f64_ne_t          = wasm_ops::f64_ne                  <instruction_counter, f64_relop_injector<wasm_ops::f64_ne_code>>;
      using f64_lt_t          = wasm_ops::f64_lt                  <instruction_counter, f64_relop_injector<wasm_ops::f64_lt_code>>;
      using f64_le_t          = wasm_ops::f64_le                  <instruction_counter, f64_relop_injector<wasm_ops::f64_le_code>>;
      using f64_gt_t          = wasm_ops::f64_gt                  <instruction_counter, f64_relop_injector<wasm_ops::f64_gt_code>>;
      using f64_ge_t          = wasm_ops::f64_ge                  <instruction_counter, f64_relop_injector<wasm_ops::f64_ge_code>>;


      using f64_promote_f32_t = wasm_ops::f64_promote_f32         <instruction_counter, f32_promote_injector>;
      using f32_demote_f64_t  = wasm_ops::f32_demote_f64          <instruction_counter, f64_demote_injector>;

      
      using i32_trunc_s_f32_t = wasm_ops::i32_trunc_s_f32         <instruction_counter, f32_trunc_i32_injector<wasm_ops::i32_trunc_s_f32_code>>;
      using i32_trunc_u_f32_t = wasm_ops::i32_trunc_u_f32         <instruction_counter, f32_trunc_i32_injector<wasm_ops::i32_trunc_u_f32_code>>;
      using i32_trunc_s_f64_t = wasm_ops::i32_trunc_s_f64         <instruction_counter, f64_trunc_i32_injector<wasm_ops::i32_trunc_s_f64_code>>;
      using i32_trunc_u_f64_t = wasm_ops::i32_trunc_u_f64         <instruction_counter, f64_trunc_i32_injector<wasm_ops::i32_trunc_u_f64_code>>;
      using i64_trunc_s_f32_t = wasm_ops::i64_trunc_s_f32         <instruction_counter, f32_trunc_i64_injector<wasm_ops::i64_trunc_s_f32_code>>;
      using i64_trunc_u_f32_t = wasm_ops::i64_trunc_u_f32         <instruction_counter, f32_trunc_i64_injector<wasm_ops::i64_trunc_u_f32_code>>;
      using i64_trunc_s_f64_t = wasm_ops::i64_trunc_s_f64         <instruction_counter, f64_trunc_i64_injector<wasm_ops::i64_trunc_s_f64_code>>;
      using i64_trunc_u_f64_t = wasm_ops::i64_trunc_u_f64         <instruction_counter, f64_trunc_i64_injector<wasm_ops::i64_trunc_u_f64_code>>;
   
      using f32_convert_s_i32 = wasm_ops::f32_convert_s_i32       <instruction_counter, i32_convert_f32_injector<wasm_ops::f32_convert_s_i32_code>>;
      using f32_convert_s_i64 = wasm_ops::f32_convert_s_i64       <instruction_counter, i64_convert_f32_injector<wasm_ops::f32_convert_s_i64_code>>;
      using f32_convert_u_i32 = wasm_ops::f32_convert_u_i32       <instruction_counter, i32_convert_f32_injector<wasm_ops::f32_convert_u_i32_code>>;
      using f32_convert_u_i64 = wasm_ops::f32_convert_u_i64       <instruction_counter, i64_convert_f32_injector<wasm_ops::f32_convert_u_i64_code>>;
      using f64_convert_s_i32 = wasm_ops::f64_convert_s_i32       <instruction_counter, i32_convert_f64_injector<wasm_ops::f64_convert_s_i32_code>>;
      using f64_convert_s_i64 = wasm_ops::f64_convert_s_i64       <instruction_counter, i64_convert_f64_injector<wasm_ops::f64_convert_s_i64_code>>;
      using f64_convert_u_i32 = wasm_ops::f64_convert_u_i32       <instruction_counter, i32_convert_f64_injector<wasm_ops::f64_convert_u_i32_code>>;
      using f64_convert_u_i64 = wasm_ops::f64_convert_u_i64       <instruction_counter, i64_convert_f64_injector<wasm_ops::f64_convert_u_i64_code>>;

      using i32_wrap_i64_t     = wasm_ops::i32_wrap_i64           <instruction_counter>;
      using i64_extend_s_i32_t = wasm_ops::i64_extend_s_i32       <instruction_counter>;
      using i64_extend_u_i32_t = wasm_ops::i64_extend_u_i32       <instruction_counter>;

      using i32_reinterpret_f32_t = wasm_ops::i32_reinterpret_f32 <instruction_counter>;
      using f32_reinterpret_i32_t = wasm_ops::f32_reinterpret_i32 <instruction_counter>;
      using i64_reinterpret_f64_t = wasm_ops::i64_reinterpret_f64 <instruction_counter>;
      using f64_reinterpret_i64_t = wasm_ops::f64_reinterpret_i64 <instruction_counter>;

   }; // pre_op_injectors


   struct post_op_injectors : wasm_ops::op_types<pass_injector> {
      using loop_t   = wasm_ops::loop        <checktime_injection>;
      using call_t   = wasm_ops::call        <fix_call_index>;
   };

   template <typename ... Visitors>
   struct module_injectors {
      static void inject( IR::Module& m ) {
         for ( auto injector : { Visitors::inject... } ) {
            injector( m );
         }
      }
      static void init() {
         // place initial values for static fields of injectors here
         for ( auto initializer : { Visitors::initializer... } ) {
            initializer();
         }
      }
   };
 
   // inherit from this class and define your own injectors 
   class wasm_binary_injection {
      using standard_module_injectors = module_injectors< max_memory_injection_visitor >;

      public:
         wasm_binary_injection( IR::Module& mod )  : _module( &mod ) { 
            _module_injectors.init();
            // initialize static fields of injectors
            injector_utils::init( mod );
            instruction_counter::init();
            checktime_injection::init();
            checktime_block_type::init();
            checktime_function_end::init();
            call_depth_check::init();
         }

         void inject() {
            _module_injectors.inject( *_module );
            // inject checktime first
            injector_utils::add_import<ResultType::none, ValueType::i32>( *_module, u8"checktime", checktime_injection::chktm_idx );

            for ( auto& fd : _module->functions.defs ) {
               wasm_ops::EOSIO_OperatorDecoderStream<pre_op_injectors> pre_decoder(fd.code);
               wasm_ops::instruction_stream pre_code(fd.code.size()*2);
               checktime_block_type::orderings.emplace();
               checktime_block_type::bcnt_tables.emplace();
               instruction_counter::icnt = 0;
               instruction_counter::tcnt = 0;
               instruction_counter::bcnt = 0;
               while ( pre_decoder ) {
                  auto op = pre_decoder.decodeOp();
                  if (op->is_post()) {
                     op->pack(&pre_code);
                     op->visit( { _module, &pre_code, &fd, pre_decoder.index() } );
                  }
                  else {
                     op->visit( { _module, &pre_code, &fd, pre_decoder.index() } );
                     if (!(op->is_kill()))
                        op->pack(&pre_code);
                  }
               }
               instruction_counter::fcnts.push(instruction_counter::tcnt - instruction_counter::bcnt);
               fd.code = pre_code.get();
            }
            for ( auto& fd : _module->functions.defs ) {
               wasm_ops::EOSIO_OperatorDecoderStream<post_op_injectors> post_decoder(fd.code);
               wasm_ops::instruction_stream post_code(fd.code.size()*2);
               bool is_start = true;
               while ( post_decoder ) {
                  auto op = post_decoder.decodeOp();
                  if ( is_start ) {
                     is_start = false;
                     wasm_ops::op_types<>::i32_const_t cnt; 
                     cnt.field = instruction_counter::fcnts.front();
                     wasm_ops::op_types<>::call_t chktm; 
                     chktm.field = injector_utils::injected_index_mapping.find(checktime_injection::chktm_idx)->second;
                     cnt.pack(&post_code);
                     chktm.pack(&post_code);
                  }
                  if (op->is_post()) {
                     op->pack(&post_code);
                     op->visit( { _module, &post_code, &fd, post_decoder.index() } );
                  }
                  else {
                     op->visit( { _module, &post_code, &fd, post_decoder.index() } );
                     if (!(op->is_kill()))
                        op->pack(&post_code);
                  }
               }
               fd.code = post_code.get();
               instruction_counter::fcnts.pop();
               checktime_block_type::orderings.pop();
               checktime_block_type::bcnt_tables.pop();
               checktime_injection::idx = 0;
            }
         }
      private:
         IR::Module* _module;
         static std::string op_string;
         static standard_module_injectors _module_injectors;
   };

}}} // namespace wasm_constraints, chain, eosio
