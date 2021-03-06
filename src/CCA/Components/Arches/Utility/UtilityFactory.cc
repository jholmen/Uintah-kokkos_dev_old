#include <CCA/Components/Arches/Utility/UtilityFactory.h>
#include <CCA/Components/Arches/Utility/GridInfo.h>
#include <CCA/Components/Arches/Utility/TaskAlgebra.h>
#include <CCA/Components/Arches/Utility/SurfaceNormals.h>
#include <CCA/Components/Arches/Task/TaskInterface.h>

using namespace Uintah;

UtilityFactory::UtilityFactory()
{}

UtilityFactory::~UtilityFactory()
{}

void
UtilityFactory::register_all_tasks( ProblemSpecP& db )
{

  //GRID INFORMATION
  std::string tname = "grid_info";
  TaskInterface::TaskBuilder* tsk = scinew GridInfo::Builder( tname, 0 );
  register_task( tname, tsk );

  tname = "surface_normals";
  tsk = scinew SurfaceNormals::Builder( tname, 0 );
  register_task( tname, tsk );

  ProblemSpecP db_all_util = db->findBlock("Utilities");

  //<Utilities>
  if ( db_all_util ){
    for ( ProblemSpecP db_util = db_all_util->findBlock("utility"); db_util != 0;
          db_util = db_util->findNextBlock("utility")){

      std::string name;
      std::string type;
      db_util->getAttribute("label", name);
      db_util->getAttribute("type", type);

      if ( type == "variable_math" ){

        //Assume all SVOl for now:
        //otherwise would need to determine or parse for the variable types
        typedef SpatialOps::SVolField SVol;
        TaskInterface::TaskBuilder* tsk = scinew TaskAlgebra<SVol,SVol,SVol>::Builder( name, 0 );
        register_task(name, tsk);

      } else {

        throw InvalidValue("Error: Utility type not recognized.",__FILE__,__LINE__);

      }

      assign_task_to_type_storage(name, type);

    }
  }
}

void
UtilityFactory::add_task( ProblemSpecP& db ){

  ProblemSpecP db_all_util = db->findBlock("Utilities");

  //<Utilities>
  if ( db_all_util ){
    for ( ProblemSpecP db_util = db_all_util->findBlock("utility"); db_util != 0;
          db_util = db_util->findNextBlock("utility")){

      std::string name;
      std::string type;
      db_util->getAttribute("label", name);
      db_util->getAttribute("type", type);

      if ( type == "variable_math" ){

        //Assume all SVOl for now:
        //otherwise would need to determine or parse for the variable types
        typedef SpatialOps::SVolField SVol;
        TaskInterface::TaskBuilder* tsk_builder = scinew TaskAlgebra<SVol,SVol,SVol>::Builder( name, 0 );
        register_task(name, tsk_builder);

        //also must build it here:
        // TaskInterface* tsk = retrieve_task(name);
        // tsk->problemSetup(db_util);
        // tsk->create_local_labels();

      } else {

        throw InvalidValue("Error: Utility type not recognized.",__FILE__,__LINE__);

      }

      assign_task_to_type_storage(name, type);

    }
  }
}

void
UtilityFactory::build_all_tasks( ProblemSpecP& db )
{

  // Grid spacing information
  TaskInterface* tsk = retrieve_task("grid_info");
  tsk->problemSetup(db);
  tsk->create_local_labels();

  tsk = retrieve_task("surface_normals");
  tsk->problemSetup(db);
  tsk->create_local_labels();

  //<Utilities>
  ProblemSpecP db_all_util = db->findBlock("Utilities");
  if ( db_all_util ){
    for ( ProblemSpecP db_util = db_all_util->findBlock("utility"); db_util != 0;
          db_util = db_util->findNextBlock("utility")){
      std::string name;
      db_util->getAttribute("label", name);
      TaskInterface* tsk = retrieve_task(name);
      tsk->problemSetup(db_util);
      tsk->create_local_labels();
    }
  }
}
