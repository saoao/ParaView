/*=========================================================================

  Program:   ParaView
  Module:    vtkSMExtractsController.cxx

  Copyright (c) Kitware, Inc.
  All rights reserved.
  See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkSMExtractsController.h"

#include "vtkCollection.h"
#include "vtkCollectionRange.h"
#include "vtkMultiProcessController.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPVProxyDefinitionIterator.h"
#include "vtkProcessModule.h"
#include "vtkSMExtractTriggerProxy.h"
#include "vtkSMExtractWriterProxy.h"
#include "vtkSMOutputPort.h"
#include "vtkSMParaViewPipelineController.h"
#include "vtkSMProperty.h"
#include "vtkSMProxyDefinitionManager.h"
#include "vtkSMProxyIterator.h"
#include "vtkSMProxyListDomain.h"
#include "vtkSMSession.h"
#include "vtkSMSessionProxyManager.h"
#include "vtkSMSourceProxy.h"
#include "vtkSMTrace.h"
#include "vtkSMUncheckedPropertyHelper.h"

#include <vtksys/SystemTools.hxx>

vtkStandardNewMacro(vtkSMExtractsController);
//----------------------------------------------------------------------------
vtkSMExtractsController::vtkSMExtractsController()
  : TimeStep(0)
  , Time(0.0)
  , DataExtractsOutputDirectory(nullptr)
  , ImageExtractsOutputDirectory(nullptr)
{
}

//----------------------------------------------------------------------------
vtkSMExtractsController::~vtkSMExtractsController()
{
  this->SetDataExtractsOutputDirectory(nullptr);
  this->SetImageExtractsOutputDirectory(nullptr);
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::Extract()
{
  if (auto session = vtkSMSession::SafeDownCast(vtkProcessModule::GetProcessModule()->GetSession()))
  {
    return this->Extract(session->GetSessionProxyManager());
  }
  vtkErrorMacro("No active session!");
  return false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::Extract(vtkSMSessionProxyManager* pxm)
{
  bool status = false;
  vtkNew<vtkSMProxyIterator> piter;
  piter->SetSessionProxyManager(pxm);
  for (piter->Begin("extract_generators"); !piter->IsAtEnd(); piter->Next())
  {
    status = this->Extract(piter->GetProxy()) || status;
  }
  return status;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::Extract(vtkCollection* collection)
{
  bool status = false;
  auto range = vtk::Range(collection);
  for (auto item : range)
  {
    if (auto generator = vtkSMProxy::SafeDownCast(item))
    {
      status = this->Extract(generator) || status;
    }
  }
  return status;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::Extract(vtkSMProxy* extractor)
{
  if (!this->IsTriggerActivated(extractor))
  {
    // skipping, nothing to do.
    return false;
  }

  // Update filenames etc.
  if (auto writer = vtkSMExtractWriterProxy::SafeDownCast(
        vtkSMPropertyHelper(extractor, "Writer").GetAsProxy(0)))
  {
    return writer->Write(this);
  }

  return false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::IsAnyTriggerActivated(vtkSMSessionProxyManager* pxm)
{
  vtkNew<vtkSMProxyIterator> piter;
  piter->SetSessionProxyManager(pxm);
  for (piter->Begin("extract_generators"); !piter->IsAtEnd(); piter->Next())
  {
    if (this->IsTriggerActivated(piter->GetProxy()))
    {
      return true;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::IsAnyTriggerActivated()
{
  if (auto session = vtkSMSession::SafeDownCast(vtkProcessModule::GetProcessModule()->GetSession()))
  {
    return this->IsAnyTriggerActivated(session->GetSessionProxyManager());
  }
  vtkErrorMacro("No active session!");
  return false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::IsAnyTriggerActivated(vtkCollection* collection)
{
  auto range = vtk::Range(collection);
  for (auto item : range)
  {
    if (auto generator = vtkSMProxy::SafeDownCast(item))
    {
      if (this->IsTriggerActivated(generator))
      {
        return true;
      }
    }
  }
  return false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::IsTriggerActivated(vtkSMProxy* extractor)
{
  if (!extractor)
  {
    vtkErrorMacro("Invalid 'extractor'.");
    return false;
  }

  if (vtkSMPropertyHelper(extractor, "EnableExtract").GetAsInt() != 1)
  {
    // skipping, not enabled.
    return false;
  }

  auto trigger =
    vtkSMExtractTriggerProxy::SafeDownCast(vtkSMPropertyHelper(extractor, "Trigger").GetAsProxy(0));
  // note, if no trigger is provided, we assume it is always enabled.
  if (trigger != nullptr && !trigger->IsActivated(this))
  {
    // skipping, nothing to do.
    return false;
  }

  return true;
}

//----------------------------------------------------------------------------
std::vector<vtkSMProxy*> vtkSMExtractsController::FindExtractGenerators(vtkSMProxy* proxy) const
{
  std::vector<vtkSMProxy*> result;
  if (!proxy)
  {
    return result;
  }

  auto pxm = proxy->GetSessionProxyManager();
  vtkNew<vtkSMProxyIterator> piter;
  piter->SetSessionProxyManager(pxm);
  for (piter->Begin("extract_generators"); !piter->IsAtEnd(); piter->Next())
  {
    auto generator = piter->GetProxy();
    if (vtkSMExtractsController::IsExtractGenerator(generator, proxy))
    {
      result.push_back(generator);
    }
  }
  return result;
}

//----------------------------------------------------------------------------
std::vector<vtkSMProxy*> vtkSMExtractsController::GetSupportedExtractGeneratorPrototypes(
  vtkSMProxy* proxy) const
{
  std::vector<vtkSMProxy*> result;
  if (!proxy)
  {
    return result;
  }

  auto pxm = proxy->GetSessionProxyManager();
  auto pdm = pxm->GetProxyDefinitionManager();
  auto piter = vtkSmartPointer<vtkPVProxyDefinitionIterator>::Take(
    pdm->NewSingleGroupIterator("extract_writers"));
  for (piter->InitTraversal(); !piter->IsDoneWithTraversal(); piter->GoToNextItem())
  {
    auto prototype = vtkSMExtractWriterProxy::SafeDownCast(
      pxm->GetPrototypeProxy("extract_writers", piter->GetProxyName()));
    if (prototype && prototype->CanExtract(proxy))
    {
      result.push_back(prototype);
    }
  }
  return result;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::CanExtract(
  vtkSMProxy* generator, const std::vector<vtkSMProxy*>& inputs) const
{
  if (!generator || inputs.size() == 0)
  {
    return false;
  }

  // currently, we don't have any multiple input extract generators.
  if (inputs.size() > 1)
  {
    return false;
  }

  // let's support the case where the 'generator' is not really the generator
  // but a writer prototype.
  auto writer = vtkSMExtractWriterProxy::SafeDownCast(generator);
  if (!writer)
  {
    writer =
      vtkSMExtractWriterProxy::SafeDownCast(vtkSMPropertyHelper(generator, "Writer").GetAsProxy());
  }

  return (writer && writer->CanExtract(inputs[0]));
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::IsExtractGenerator(vtkSMProxy* generator, vtkSMProxy* proxy)
{
  if (proxy == nullptr || generator == nullptr)
  {
    return false;
  }

  auto writer =
    vtkSMExtractWriterProxy::SafeDownCast(vtkSMPropertyHelper(generator, "Writer").GetAsProxy());
  return (writer && writer->IsExtracting(proxy)) ? true : false;
}

//----------------------------------------------------------------------------
vtkSMProxy* vtkSMExtractsController::GetInputForGenerator(vtkSMProxy* generator) const
{
  if (generator == nullptr)
  {
    return nullptr;
  }

  auto writer =
    vtkSMExtractWriterProxy::SafeDownCast(vtkSMPropertyHelper(generator, "Writer").GetAsProxy());
  return (writer ? writer->GetInput() : nullptr);
}

//----------------------------------------------------------------------------
vtkSMProxy* vtkSMExtractsController::CreateExtractGenerator(
  vtkSMProxy* proxy, const char* xmlname, const char* registrationName /*=nullptr*/) const
{
  if (!proxy)
  {
    return nullptr;
  }

  vtkNew<vtkSMParaViewPipelineController> controller;
  auto pxm = proxy->GetSessionProxyManager();
  auto writer = vtkSmartPointer<vtkSMExtractWriterProxy>::Take(
    vtkSMExtractWriterProxy::SafeDownCast(pxm->NewProxy("extract_writers", xmlname)));
  if (!writer)
  {
    return nullptr;
  }

  const std::string pname = registrationName
    ? std::string(registrationName)
    : pxm->GetUniqueProxyName("extract_generators", writer->GetXMLLabel());
  auto generator =
    vtkSmartPointer<vtkSMProxy>::Take(pxm->NewProxy("extract_generators", "Extractor"));
  // add it to the proxy list domain too so the UI shows it correctly.
  if (auto prop = generator->GetProperty("Writer"))
  {
    if (auto pld = prop->FindDomain<vtkSMProxyListDomain>())
    {
      pld->AddProxy(writer);
    }
  }

  SM_SCOPED_TRACE(CreateExtractGenerator)
    .arg("producer", proxy)
    .arg("generator", generator)
    .arg("xmlname", xmlname)
    .arg("registrationName", pname.c_str());

  controller->PreInitializeProxy(generator);
  writer->SetInput(proxy);
  vtkSMPropertyHelper(generator, "Writer").Set(writer);

  // this is done so that producer-consumer links are setup properly. Makes it easier
  // to delete the Extractor when the producer goes away.
  if (auto port = vtkSMOutputPort::SafeDownCast(proxy))
  {
    vtkSMPropertyHelper(generator, "Producer").Set(port->GetSourceProxy());
  }
  else
  {
    vtkSMPropertyHelper(generator, "Producer").Set(proxy);
  }
  controller->PostInitializeProxy(generator);
  generator->UpdateVTKObjects();
  controller->RegisterExtractGeneratorProxy(generator, pname.c_str());

  return generator;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::CreateImageExtractsOutputDirectory() const
{
  return this->ImageExtractsOutputDirectory
    ? this->CreateDirectory(this->ImageExtractsOutputDirectory)
    : false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::CreateDataExtractsOutputDirectory() const
{
  return this->DataExtractsOutputDirectory
    ? this->CreateDirectory(this->DataExtractsOutputDirectory)
    : false;
}

//----------------------------------------------------------------------------
bool vtkSMExtractsController::CreateDirectory(const std::string& dname) const
{
  if (dname.empty())
  {
    return false;
  }

  auto controller = vtkMultiProcessController::GetGlobalController();
  if (controller)
  {
    int status = 0;
    if (controller->GetLocalProcessId() == 0)
    {
      status = vtksys::SystemTools::MakeDirectory(dname) ? 1 : 0;
    }
    controller->Broadcast(&status, 1, 0);
    return (status == 1);
  }
  else
  {
    return vtksys::SystemTools::MakeDirectory(dname);
  }
}

//----------------------------------------------------------------------------
void vtkSMExtractsController::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "TimeStep: " << this->TimeStep << endl;
  os << indent << "Time: " << this->Time << endl;
}
