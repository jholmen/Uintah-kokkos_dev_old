#ifndef Uintah_Component_Arches_InitLagrangianParticleSize_h
#define Uintah_Component_Arches_InitLagrangianParticleSize_h

#include <CCA/Components/Arches/Task/TaskInterface.h>

namespace Uintah{ 

  class Operators; 
  class InitLagrangianParticleSize : public TaskInterface { 

public: 

    InitLagrangianParticleSize( std::string task_name, int matl_index ); 
    ~InitLagrangianParticleSize(); 

    void problemSetup( ProblemSpecP& db ); 

    void register_initialize( std::vector<ArchesFieldContainer::VariableInformation>& variable_registry );

    void register_timestep_init( std::vector<ArchesFieldContainer::VariableInformation>& variable_registry ); 

    void register_timestep_eval( std::vector<ArchesFieldContainer::VariableInformation>& variable_registry, const int time_substep ); 

    void register_compute_bcs( std::vector<ArchesFieldContainer::VariableInformation>& variable_registry, const int time_substep ){}; 

    void compute_bcs( const Patch* patch, ArchesTaskInfoManager* tsk_info, 
                      SpatialOps::OperatorDatabase& opr ){}; 

    void initialize( const Patch* patch, ArchesTaskInfoManager* tsk_info, 
                     SpatialOps::OperatorDatabase& opr );
    
    void timestep_init( const Patch* patch, ArchesTaskInfoManager* tsk_info, 
                        SpatialOps::OperatorDatabase& opr );

    void eval( const Patch* patch, ArchesTaskInfoManager* tsk_info, 
               SpatialOps::OperatorDatabase& opr ); 

    void create_local_labels();

    //Build instructions for this (InitLagrangianParticleSize) class. 
    class Builder : public TaskInterface::TaskBuilder { 

      public: 

      Builder( std::string task_name, int matl_index ) : _task_name(task_name), _matl_index(matl_index){}
      ~Builder(){}

      InitLagrangianParticleSize* build()
      { return scinew InitLagrangianParticleSize( _task_name, _matl_index ); }

      private: 

      std::string _task_name; 
      int _matl_index; 

    };

private: 

    std::string _pu_label; 
    std::string _pv_label; 
    std::string _pw_label; 

    std::string _px_label; 
    std::string _py_label; 
    std::string _pz_label; 

    std::string _size_label; 
    std::string _init_type; 
    
    double _fixed_d; ///< Fixed diameter
    double _max_d;   ///< max allowed diameter
  
  };
}
#endif 
