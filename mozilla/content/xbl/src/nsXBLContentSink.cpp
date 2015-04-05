/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Util.h"

#include "nsXBLContentSink.h"
#include "nsIDocument.h"
#include "nsBindingManager.h"
#include "nsIDOMNode.h"
#include "nsGkAtoms.h"
#include "nsINameSpaceManager.h"
#include "nsHTMLTokens.h"
#include "nsIURI.h"
#include "nsTextFragment.h"
#ifdef MOZ_XUL
#include "nsXULElement.h"
#endif
#include "nsXBLProtoImplProperty.h"
#include "nsXBLProtoImplMethod.h"
#include "nsXBLProtoImplField.h"
#include "nsXBLPrototypeBinding.h"
#include "nsContentUtils.h"
#include "nsIConsoleService.h"
#include "nsIScriptError.h"
#include "nsNodeInfoManager.h"
#include "nsINodeInfo.h"
#include "nsIPrincipal.h"
#include "mozilla/dom/Element.h"

using namespace mozilla;
using namespace mozilla::dom;

nsresult
NS_NewXBLContentSink(nsIXMLContentSink** aResult,
                     nsIDocument* aDoc,
                     nsIURI* aURI,
                     nsISupports* aContainer)
{
  NS_ENSURE_ARG_POINTER(aResult);

  nsXBLContentSink* it = new nsXBLContentSink();
  NS_ENSURE_TRUE(it, NS_ERROR_OUT_OF_MEMORY);

  nsCOMPtr<nsIXMLContentSink> kungFuDeathGrip = it;
  nsresult rv = it->Init(aDoc, aURI, aContainer);
  NS_ENSURE_SUCCESS(rv, rv);

  return CallQueryInterface(it, aResult);
}

nsXBLContentSink::nsXBLContentSink()
  : mState(eXBL_InDocument),
    mSecondaryState(eXBL_None),
    mDocInfo(nullptr),
    mIsChromeOrResource(false),
    mFoundFirstBinding(false),    
    mBinding(nullptr),
    mHandler(nullptr),
    mImplementation(nullptr),
    mImplMember(nullptr),
    mImplField(nullptr),
    mProperty(nullptr),
    mMethod(nullptr),
    mField(nullptr)
{
  mPrettyPrintXML = false;
}

nsXBLContentSink::~nsXBLContentSink()
{
}

nsresult
nsXBLContentSink::Init(nsIDocument* aDoc,
                       nsIURI* aURI,
                       nsISupports* aContainer)
{
  nsresult rv;
  rv = nsXMLContentSink::Init(aDoc, aURI, aContainer, nullptr);
  return rv;
}

void
nsXBLContentSink::MaybeStartLayout(bool aIgnorePendingSheets)
{
  return;
}

nsresult
nsXBLContentSink::FlushText(bool aReleaseTextNode)
{
  if (mTextLength != 0) {
    const nsASingleFragmentString& text = Substring(mText, mText+mTextLength);
    if (mState == eXBL_InHandlers) {
      NS_ASSERTION(mBinding, "Must have binding here");
      // Get the text and add it to the event handler.
      if (mSecondaryState == eXBL_InHandler)
        mHandler->AppendHandlerText(text);
      mTextLength = 0;
      return NS_OK;
    }
    else if (mState == eXBL_InImplementation) {
      NS_ASSERTION(mBinding, "Must have binding here");
      if (mSecondaryState == eXBL_InConstructor ||
          mSecondaryState == eXBL_InDestructor) {
        // Construct a method for the constructor/destructor.
        nsXBLProtoImplMethod* method;
        if (mSecondaryState == eXBL_InConstructor)
          method = mBinding->GetConstructor();
        else
          method = mBinding->GetDestructor();

        // Get the text and add it to the constructor/destructor.
        method->AppendBodyText(text);
      }
      else if (mSecondaryState == eXBL_InGetter ||
               mSecondaryState == eXBL_InSetter) {
        // Get the text and add it to the getter/setter
        if (mSecondaryState == eXBL_InGetter)
          mProperty->AppendGetterText(text);
        else
          mProperty->AppendSetterText(text);
      }
      else if (mSecondaryState == eXBL_InBody) {
        // Get the text and add it to the method
        if (mMethod)
          mMethod->AppendBodyText(text);
      }
      else if (mSecondaryState == eXBL_InField) {
        // Get the text and add it to the method
        if (mField)
          mField->AppendFieldText(text);
      }
      mTextLength = 0;
      return NS_OK;
    }

    nsIContent* content = GetCurrentContent();
    if (content &&
        (content->NodeInfo()->NamespaceEquals(kNameSpaceID_XBL) ||
         (content->NodeInfo()->NamespaceEquals(kNameSpaceID_XUL) &&
          content->Tag() != nsGkAtoms::label &&
          content->Tag() != nsGkAtoms::description))) {

      bool isWS = true;
      if (mTextLength > 0) {
        const PRUnichar* cp = mText;
        const PRUnichar* end = mText + mTextLength;
        while (cp < end) {
          PRUnichar ch = *cp++;
          if (!XP_IS_SPACE(ch)) {
            isWS = false;
            break;
          }
        }
      }

      if (isWS && mTextLength > 0) {
        mTextLength = 0;
        // Make sure to drop the textnode, if any
        return nsXMLContentSink::FlushText(aReleaseTextNode);
      }
    }
  }

  return nsXMLContentSink::FlushText(aReleaseTextNode);
}

NS_IMETHODIMP
nsXBLContentSink::ReportError(const PRUnichar* aErrorText, 
                              const PRUnichar* aSourceText,
                              nsIScriptError *aError,
                              bool *_retval)
{
  NS_PRECONDITION(aError && aSourceText && aErrorText, "Check arguments!!!");

  // XXX FIXME This function overrides and calls on
  // nsXMLContentSink::ReportError, and probably should die.  See bug 347826.

  // XXX We should make sure the binding has no effect, but that it also
  // gets destroyed properly without leaking.  Perhaps we should even
  // ensure that the content that was bound is displayed with no
  // binding.

#ifdef DEBUG
  // Report the error to stderr.
  fprintf(stderr,
          "\n%s\n%s\n\n",
          NS_LossyConvertUTF16toASCII(aErrorText).get(),
          NS_LossyConvertUTF16toASCII(aSourceText).get());
#endif

  // Most of what this does won't be too useful, but whatever...
  // nsXMLContentSink::ReportError will handle the console logging.
  return nsXMLContentSink::ReportError(aErrorText, 
                                       aSourceText, 
                                       aError,
                                       _retval);
}

nsresult
nsXBLContentSink::ReportUnexpectedElement(nsIAtom* aElementName,
                                          uint32_t aLineNumber)
{
  // XXX we should really somehow stop the parse and drop the binding
  // instead of just letting the XML sink build the content model like
  // we do...
  mState = eXBL_Error;
  nsAutoString elementName;
  aElementName->ToString(elementName);

  const PRUnichar* params[] = { elementName.get() };

  return nsContentUtils::ReportToConsole(nsIScriptError::errorFlag,
                                         "XBL Content Sink",
                                         mDocument,
                                         nsContentUtils::eXBL_PROPERTIES,
                                         "UnexpectedElement",
                                         params, ArrayLength(params),
                                         nullptr,
                                         EmptyString() /* source line */,
                                         aLineNumber);
}

void
nsXBLContentSink::AddMember(nsXBLProtoImplMember* aMember)
{
  // Add this member to our chain.
  if (mImplMember)
    mImplMember->SetNext(aMember); // Already have a chain. Just append to the end.
  else
    mImplementation->SetMemberList(aMember); // We're the first member in the chain.

  mImplMember = aMember; // Adjust our pointer to point to the new last member in the chain.
}

void
nsXBLContentSink::AddField(nsXBLProtoImplField* aField)
{
  // Add this field to our chain.
  if (mImplField)
    mImplField->SetNext(aField); // Already have a chain. Just append to the end.
  else
    mImplementation->SetFieldList(aField); // We're the first member in the chain.

  mImplField = aField; // Adjust our pointer to point to the new last field in the chain.
}

NS_IMETHODIMP 
nsXBLContentSink::HandleStartElement(const PRUnichar *aName, 
                                     const PRUnichar **aAtts, 
                                     uint32_t aAttsCount, 
                                     int32_t aIndex, 
                                     uint32_t aLineNumber)
{
  nsresult rv = nsXMLContentSink::HandleStartElement(aName,aAtts,aAttsCount,aIndex,aLineNumber);
  if (NS_FAILED(rv))
    return rv;

  if (mState == eXBL_InBinding && !mBinding) {
    rv = ConstructBinding(aLineNumber);
    if (NS_FAILED(rv))
      return rv;
    
    // mBinding may still be null, if the binding had no id.  If so,
    // we'll deal with that later in the sink.
  }

  return rv;
}

NS_IMETHODIMP 
nsXBLContentSink::HandleEndElement(const PRUnichar *aName)
{
  FlushText();

  if (mState != eXBL_InDocument) {
    int32_t nameSpaceID;
    nsCOMPtr<nsIAtom> prefix, localName;
    nsContentUtils::SplitExpatName(aName, getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID == kNameSpaceID_XBL) {
      if (mState == eXBL_Error) {
        // Check whether we've opened this tag before; we may not have if
        // it was a real XBL tag before the error occurred.
        if (!GetCurrentContent()->NodeInfo()->Equals(localName,
                                                     nameSpaceID)) {
          // OK, this tag was never opened as far as the XML sink is
          // concerned.  Just drop the HandleEndElement
          return NS_OK;
        }
      }
      else if (mState == eXBL_InHandlers) {
        if (localName == nsGkAtoms::handlers) {
          mState = eXBL_InBinding;
          mHandler = nullptr;
        }
        else if (localName == nsGkAtoms::handler)
          mSecondaryState = eXBL_None;
        return NS_OK;
      }
      else if (mState == eXBL_InResources) {
        if (localName == nsGkAtoms::resources)
          mState = eXBL_InBinding;
        return NS_OK;
      }
      else if (mState == eXBL_InImplementation) {
        if (localName == nsGkAtoms::implementation)
          mState = eXBL_InBinding;
        else if (localName == nsGkAtoms::property) {
          mSecondaryState = eXBL_None;
          mProperty = nullptr;
        }
        else if (localName == nsGkAtoms::method) {
          mSecondaryState = eXBL_None;
          mMethod = nullptr;
        }
        else if (localName == nsGkAtoms::field) {
          mSecondaryState = eXBL_None;
          mField = nullptr;
        }
        else if (localName == nsGkAtoms::constructor ||
                 localName == nsGkAtoms::destructor)
          mSecondaryState = eXBL_None;
        else if (localName == nsGkAtoms::getter ||
                 localName == nsGkAtoms::setter)
          mSecondaryState = eXBL_InProperty;
        else if (localName == nsGkAtoms::parameter ||
                 localName == nsGkAtoms::body)
          mSecondaryState = eXBL_InMethod;
        return NS_OK;
      }
      else if (mState == eXBL_InBindings &&
               localName == nsGkAtoms::bindings) {
        mState = eXBL_InDocument;
      }
      
      nsresult rv = nsXMLContentSink::HandleEndElement(aName);
      if (NS_FAILED(rv))
        return rv;

      if (mState == eXBL_InBinding && localName == nsGkAtoms::binding) {
        mState = eXBL_InBindings;
        if (mBinding) {  // See comment in HandleStartElement()
          mBinding->Initialize();
          mBinding = nullptr; // Clear our current binding ref.
        }
      }

      return NS_OK;
    }
  }

  return nsXMLContentSink::HandleEndElement(aName);
}

NS_IMETHODIMP 
nsXBLContentSink::HandleCDataSection(const PRUnichar *aData, 
                                     uint32_t aLength)
{
  if (mState == eXBL_InHandlers || mState == eXBL_InImplementation)
    return AddText(aData, aLength);
  return nsXMLContentSink::HandleCDataSection(aData, aLength);
}

#define ENSURE_XBL_STATE(_cond)                                                       \
  PR_BEGIN_MACRO                                                                      \
    if (!(_cond)) { ReportUnexpectedElement(aTagName, aLineNumber); return true; } \
  PR_END_MACRO

bool 
nsXBLContentSink::OnOpenContainer(const PRUnichar **aAtts, 
                                  uint32_t aAttsCount, 
                                  int32_t aNameSpaceID, 
                                  nsIAtom* aTagName,
                                  uint32_t aLineNumber)
{
  if (mState == eXBL_Error) {
    return true;
  }
  
  if (aNameSpaceID != kNameSpaceID_XBL) {
    // Construct non-XBL nodes
    return true;
  }

  bool ret = true;
  if (aTagName == nsGkAtoms::bindings) {
    ENSURE_XBL_STATE(mState == eXBL_InDocument);

    NS_ASSERTION(mDocument, "Must have a document!");
    nsRefPtr<nsXBLDocumentInfo> info = new nsXBLDocumentInfo(mDocument);

    // We keep a weak ref. We're creating a cycle between doc/binding manager/doc info.
    mDocInfo = info;

    if (!mDocInfo) {
      mState = eXBL_Error;
      return true;
    }

    mDocument->BindingManager()->PutXBLDocumentInfo(mDocInfo);

    nsIURI *uri = mDocument->GetDocumentURI();

    bool isChrome = false;
    bool isRes = false;

    uri->SchemeIs("chrome", &isChrome);
    uri->SchemeIs("resource", &isRes);
    mIsChromeOrResource = isChrome || isRes;

    mState = eXBL_InBindings;
  }
  else if (aTagName == nsGkAtoms::binding) {
    ENSURE_XBL_STATE(mState == eXBL_InBindings);
    mState = eXBL_InBinding;
  }
  else if (aTagName == nsGkAtoms::handlers) {
    ENSURE_XBL_STATE(mState == eXBL_InBinding && mBinding);
    mState = eXBL_InHandlers;
    ret = false;
  }
  else if (aTagName == nsGkAtoms::handler) {
    ENSURE_XBL_STATE(mState == eXBL_InHandlers);
    mSecondaryState = eXBL_InHandler;
    ConstructHandler(aAtts, aLineNumber);
    ret = false;
  }
  else if (aTagName == nsGkAtoms::resources) {
    ENSURE_XBL_STATE(mState == eXBL_InBinding && mBinding);
    mState = eXBL_InResources;
    // Note that this mState will cause us to return false, so no need
    // to set ret to false.
  }
  else if (aTagName == nsGkAtoms::stylesheet || aTagName == nsGkAtoms::image) {
    ENSURE_XBL_STATE(mState == eXBL_InResources);
    NS_ASSERTION(mBinding, "Must have binding here");
    ConstructResource(aAtts, aTagName);
  }
  else if (aTagName == nsGkAtoms::implementation) {
    ENSURE_XBL_STATE(mState == eXBL_InBinding && mBinding);
    mState = eXBL_InImplementation;
    ConstructImplementation(aAtts);
    // Note that this mState will cause us to return false, so no need
    // to set ret to false.
  }
  else if (aTagName == nsGkAtoms::constructor) {
    ENSURE_XBL_STATE(mState == eXBL_InImplementation &&
                     mSecondaryState == eXBL_None);
    NS_ASSERTION(mBinding, "Must have binding here");
      
    mSecondaryState = eXBL_InConstructor;
    nsXBLProtoImplAnonymousMethod* newMethod =
      new nsXBLProtoImplAnonymousMethod();
    if (newMethod) {
      newMethod->SetLineNumber(aLineNumber);
      mBinding->SetConstructor(newMethod);
      AddMember(newMethod);
    }
  }
  else if (aTagName == nsGkAtoms::destructor) {
    ENSURE_XBL_STATE(mState == eXBL_InImplementation &&
                     mSecondaryState == eXBL_None);
    NS_ASSERTION(mBinding, "Must have binding here");
    mSecondaryState = eXBL_InDestructor;
    nsXBLProtoImplAnonymousMethod* newMethod =
      new nsXBLProtoImplAnonymousMethod();
    if (newMethod) {
      newMethod->SetLineNumber(aLineNumber);
      mBinding->SetDestructor(newMethod);
      AddMember(newMethod);
    }
  }
  else if (aTagName == nsGkAtoms::field) {
    ENSURE_XBL_STATE(mState == eXBL_InImplementation &&
                     mSecondaryState == eXBL_None);
    NS_ASSERTION(mBinding, "Must have binding here");
    mSecondaryState = eXBL_InField;
    ConstructField(aAtts, aLineNumber);
  }
  else if (aTagName == nsGkAtoms::property) {
    ENSURE_XBL_STATE(mState == eXBL_InImplementation &&
                     mSecondaryState == eXBL_None);
    NS_ASSERTION(mBinding, "Must have binding here");
    mSecondaryState = eXBL_InProperty;
    ConstructProperty(aAtts);
  }
  else if (aTagName == nsGkAtoms::getter) {
    ENSURE_XBL_STATE(mSecondaryState == eXBL_InProperty && mProperty);
    NS_ASSERTION(mState == eXBL_InImplementation, "Unexpected state");
    mProperty->SetGetterLineNumber(aLineNumber);
    mSecondaryState = eXBL_InGetter;
  }
  else if (aTagName == nsGkAtoms::setter) {
    ENSURE_XBL_STATE(mSecondaryState == eXBL_InProperty && mProperty);
    NS_ASSERTION(mState == eXBL_InImplementation, "Unexpected state");
    mProperty->SetSetterLineNumber(aLineNumber);
    mSecondaryState = eXBL_InSetter;
  }
  else if (aTagName == nsGkAtoms::method) {
    ENSURE_XBL_STATE(mState == eXBL_InImplementation &&
                     mSecondaryState == eXBL_None);
    NS_ASSERTION(mBinding, "Must have binding here");
    mSecondaryState = eXBL_InMethod;
    ConstructMethod(aAtts);
  }
  else if (aTagName == nsGkAtoms::parameter) {
    ENSURE_XBL_STATE(mSecondaryState == eXBL_InMethod && mMethod);
    NS_ASSERTION(mState == eXBL_InImplementation, "Unexpected state");
    ConstructParameter(aAtts);
  }
  else if (aTagName == nsGkAtoms::body) {
    ENSURE_XBL_STATE(mSecondaryState == eXBL_InMethod && mMethod);
    NS_ASSERTION(mState == eXBL_InImplementation, "Unexpected state");
    // stash away the line number
    mMethod->SetLineNumber(aLineNumber);
    mSecondaryState = eXBL_InBody;
  }

  return ret && mState != eXBL_InResources && mState != eXBL_InImplementation;
}

#undef ENSURE_XBL_STATE

nsresult
nsXBLContentSink::ConstructBinding(uint32_t aLineNumber)
{
  nsCOMPtr<nsIContent> binding = GetCurrentContent();
  nsAutoString id;
  binding->GetAttr(kNameSpaceID_None, nsGkAtoms::id, id);
  NS_ConvertUTF16toUTF8 cid(id);

  nsresult rv = NS_OK;

  // Don't create a binding with no id. nsXBLPrototypeBinding::Read also
  // performs this check.
  if (!cid.IsEmpty()) {
    mBinding = new nsXBLPrototypeBinding();
    if (!mBinding)
      return NS_ERROR_OUT_OF_MEMORY;
      
    rv = mBinding->Init(cid, mDocInfo, binding, !mFoundFirstBinding);
    if (NS_SUCCEEDED(rv) &&
        NS_SUCCEEDED(mDocInfo->SetPrototypeBinding(cid, mBinding))) {
      if (!mFoundFirstBinding) {
        mFoundFirstBinding = true;
        mDocInfo->SetFirstPrototypeBinding(mBinding);
      }
      binding->UnsetAttr(kNameSpaceID_None, nsGkAtoms::id, false);
    } else {
      delete mBinding;
      mBinding = nullptr;
    }
  } else {
    nsContentUtils::ReportToConsole(nsIScriptError::errorFlag,
                                    "XBL Content Sink", nullptr,
                                    nsContentUtils::eXBL_PROPERTIES,
                                    "MissingIdAttr", nullptr, 0,
                                    mDocumentURI,
                                    EmptyString(),
                                    aLineNumber);
  }

  return rv;
}

static bool
FindValue(const PRUnichar **aAtts, nsIAtom *aAtom, const PRUnichar **aResult)
{
  nsCOMPtr<nsIAtom> prefix, localName;
  for (; *aAtts; aAtts += 2) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[0], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    // Is this attribute one of the ones we care about?
    if (nameSpaceID == kNameSpaceID_None && localName == aAtom) {
      *aResult = aAtts[1];

      return true;
    }
  }

  return false;
}

void
nsXBLContentSink::ConstructHandler(const PRUnichar **aAtts, uint32_t aLineNumber)
{
  const PRUnichar* event          = nullptr;
  const PRUnichar* modifiers      = nullptr;
  const PRUnichar* button         = nullptr;
  const PRUnichar* clickcount     = nullptr;
  const PRUnichar* keycode        = nullptr;
  const PRUnichar* charcode       = nullptr;
  const PRUnichar* phase          = nullptr;
  const PRUnichar* command        = nullptr;
  const PRUnichar* action         = nullptr;
  const PRUnichar* group          = nullptr;
  const PRUnichar* preventdefault = nullptr;
  const PRUnichar* allowuntrusted = nullptr;

  nsCOMPtr<nsIAtom> prefix, localName;
  for (; *aAtts; aAtts += 2) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[0], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID != kNameSpaceID_None) {
      continue;
    }

    // Is this attribute one of the ones we care about?
    if (localName == nsGkAtoms::event)
      event = aAtts[1];
    else if (localName == nsGkAtoms::modifiers)
      modifiers = aAtts[1];
    else if (localName == nsGkAtoms::button)
      button = aAtts[1];
    else if (localName == nsGkAtoms::clickcount)
      clickcount = aAtts[1];
    else if (localName == nsGkAtoms::keycode)
      keycode = aAtts[1];
    else if (localName == nsGkAtoms::key || localName == nsGkAtoms::charcode)
      charcode = aAtts[1];
    else if (localName == nsGkAtoms::phase)
      phase = aAtts[1];
    else if (localName == nsGkAtoms::command)
      command = aAtts[1];
    else if (localName == nsGkAtoms::action)
      action = aAtts[1];
    else if (localName == nsGkAtoms::group)
      group = aAtts[1];
    else if (localName == nsGkAtoms::preventdefault)
      preventdefault = aAtts[1];
    else if (localName == nsGkAtoms::allowuntrusted)
      allowuntrusted = aAtts[1];
  }

  if (command && !mIsChromeOrResource) {
    // Make sure the XBL doc is chrome or resource if we have a command
    // shorthand syntax.
    mState = eXBL_Error;
    nsContentUtils::ReportToConsole(nsIScriptError::errorFlag,
                                    "XBL Content Sink",
                                    mDocument,
                                    nsContentUtils::eXBL_PROPERTIES,
                                    "CommandNotInChrome", nullptr, 0,
                                    nullptr,
                                    EmptyString() /* source line */,
                                    aLineNumber);
    return; // Don't even make this handler.
  }

  // All of our pointers are now filled in. Construct our handler with all of
  // these parameters.
  nsXBLPrototypeHandler* newHandler;
  newHandler = new nsXBLPrototypeHandler(event, phase, action, command,
                                         keycode, charcode, modifiers, button,
                                         clickcount, group, preventdefault,
                                         allowuntrusted, mBinding, aLineNumber);

  if (newHandler) {
    // Add this handler to our chain of handlers.
    if (mHandler) {
      // Already have a chain. Just append to the end.
      mHandler->SetNextHandler(newHandler);
    }
    else {
      // We're the first handler in the chain.
      mBinding->SetPrototypeHandlers(newHandler);
    }
    // Adjust our mHandler pointer to point to the new last handler in the
    // chain.
    mHandler = newHandler;
  } else {
    mState = eXBL_Error;
  }
}

void
nsXBLContentSink::ConstructResource(const PRUnichar **aAtts,
                                    nsIAtom* aResourceType)
{
  if (!mBinding)
    return;

  const PRUnichar* src = nullptr;
  if (FindValue(aAtts, nsGkAtoms::src, &src)) {
    mBinding->AddResource(aResourceType, nsDependentString(src));
  }
}

void
nsXBLContentSink::ConstructImplementation(const PRUnichar **aAtts)
{
  mImplementation = nullptr;
  mImplMember = nullptr;
  mImplField = nullptr;
  
  if (!mBinding)
    return;

  const PRUnichar* name = nullptr;

  nsCOMPtr<nsIAtom> prefix, localName;
  for (; *aAtts; aAtts += 2) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[0], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID != kNameSpaceID_None) {
      continue;
    }

    // Is this attribute one of the ones we care about?
    if (localName == nsGkAtoms::name) {
      name = aAtts[1];
    }
    else if (localName == nsGkAtoms::implements) {
      // Only allow implementation of interfaces via XBL if the principal of
      // our XBL document has UniversalXPConnect privileges.  No principal
      // means no privs!
      
      // XXX this api is so badly tied to JS it's not even funny.  We don't
      // have a concept of enabling capabilities on a per-principal basis,
      // but only on a per-principal-and-JS-stackframe basis!  So for now
      // this is basically equivalent to testing that we have the system
      // principal, since there is no JS stackframe in sight here...
      bool hasUniversalXPConnect;
      nsresult rv = mDocument->NodePrincipal()->
        IsCapabilityEnabled("UniversalXPConnect", nullptr,
                            &hasUniversalXPConnect);
      if (NS_SUCCEEDED(rv) && hasUniversalXPConnect) {
        mBinding->ConstructInterfaceTable(nsDependentString(aAtts[1]));
      }
    }
  }

  NS_NewXBLProtoImpl(mBinding, name, &mImplementation);
}

void
nsXBLContentSink::ConstructField(const PRUnichar **aAtts, uint32_t aLineNumber)
{
  const PRUnichar* name     = nullptr;
  const PRUnichar* readonly = nullptr;

  nsCOMPtr<nsIAtom> prefix, localName;
  for (; *aAtts; aAtts += 2) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[0], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID != kNameSpaceID_None) {
      continue;
    }

    // Is this attribute one of the ones we care about?
    if (localName == nsGkAtoms::name) {
      name = aAtts[1];
    }
    else if (localName == nsGkAtoms::readonly) {
      readonly = aAtts[1];
    }
  }

  if (name) {
    // All of our pointers are now filled in. Construct our field with all of
    // these parameters.
    mField = new nsXBLProtoImplField(name, readonly);
    if (mField) {
      mField->SetLineNumber(aLineNumber);
      AddField(mField);
    }
  }
}

void
nsXBLContentSink::ConstructProperty(const PRUnichar **aAtts)
{
  const PRUnichar* name     = nullptr;
  const PRUnichar* readonly = nullptr;
  const PRUnichar* onget    = nullptr;
  const PRUnichar* onset    = nullptr;

  nsCOMPtr<nsIAtom> prefix, localName;
  for (; *aAtts; aAtts += 2) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[0], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID != kNameSpaceID_None) {
      continue;
    }

    // Is this attribute one of the ones we care about?
    if (localName == nsGkAtoms::name) {
      name = aAtts[1];
    }
    else if (localName == nsGkAtoms::readonly) {
      readonly = aAtts[1];
    }
    else if (localName == nsGkAtoms::onget) {
      onget = aAtts[1];
    }
    else if (localName == nsGkAtoms::onset) {
      onset = aAtts[1];
    }
  }

  if (name) {
    // All of our pointers are now filled in. Construct our property with all of
    // these parameters.
    mProperty = new nsXBLProtoImplProperty(name, onget, onset, readonly);
    if (mProperty) {
      AddMember(mProperty);
    }
  }
}

void
nsXBLContentSink::ConstructMethod(const PRUnichar **aAtts)
{
  mMethod = nullptr;

  const PRUnichar* name = nullptr;
  if (FindValue(aAtts, nsGkAtoms::name, &name)) {
    mMethod = new nsXBLProtoImplMethod(name);
  }

  if (mMethod) {
    AddMember(mMethod);
  }
}

void
nsXBLContentSink::ConstructParameter(const PRUnichar **aAtts)
{
  if (!mMethod)
    return;

  const PRUnichar* name = nullptr;
  if (FindValue(aAtts, nsGkAtoms::name, &name)) {
    mMethod->AddParameter(nsDependentString(name));
  }
}

nsresult
nsXBLContentSink::CreateElement(const PRUnichar** aAtts, uint32_t aAttsCount,
                                nsINodeInfo* aNodeInfo, uint32_t aLineNumber,
                                nsIContent** aResult, bool* aAppendContent,
                                FromParser aFromParser)
{
#ifdef MOZ_XUL
  if (!aNodeInfo->NamespaceEquals(kNameSpaceID_XUL)) {
#endif
    return nsXMLContentSink::CreateElement(aAtts, aAttsCount, aNodeInfo,
                                           aLineNumber, aResult,
                                           aAppendContent, aFromParser);
#ifdef MOZ_XUL
  }

  // Note that this needs to match the code in nsXBLPrototypeBinding::ReadContentNode.

  *aAppendContent = true;
  nsRefPtr<nsXULPrototypeElement> prototype = new nsXULPrototypeElement();
  if (!prototype)
    return NS_ERROR_OUT_OF_MEMORY;

  prototype->mNodeInfo = aNodeInfo;

  AddAttributesToXULPrototype(aAtts, aAttsCount, prototype);

  Element* result;
  nsresult rv = nsXULElement::Create(prototype, mDocument, false, &result);
  *aResult = result;
  return rv;
#endif
}

nsresult 
nsXBLContentSink::AddAttributes(const PRUnichar** aAtts,
                                nsIContent* aContent)
{
  if (aContent->IsXUL())
    return NS_OK; // Nothing to do, since the proto already has the attrs.

  return nsXMLContentSink::AddAttributes(aAtts, aContent);
}

#ifdef MOZ_XUL
nsresult
nsXBLContentSink::AddAttributesToXULPrototype(const PRUnichar **aAtts, 
                                              uint32_t aAttsCount, 
                                              nsXULPrototypeElement* aElement)
{
  // Add tag attributes to the element
  nsresult rv;

  // Create storage for the attributes
  nsXULPrototypeAttribute* attrs = nullptr;
  if (aAttsCount > 0) {
    attrs = new nsXULPrototypeAttribute[aAttsCount];
    if (!attrs)
      return NS_ERROR_OUT_OF_MEMORY;
  }

  aElement->mAttributes    = attrs;
  aElement->mNumAttributes = aAttsCount;

  // Copy the attributes into the prototype
  nsCOMPtr<nsIAtom> prefix, localName;

  uint32_t i;  
  for (i = 0; i < aAttsCount; ++i) {
    int32_t nameSpaceID;
    nsContentUtils::SplitExpatName(aAtts[i * 2], getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    if (nameSpaceID == kNameSpaceID_None) {
      attrs[i].mName.SetTo(localName);
    }
    else {
      nsCOMPtr<nsINodeInfo> ni;
      ni = mNodeInfoManager->GetNodeInfo(localName, prefix, nameSpaceID,
                                         nsIDOMNode::ATTRIBUTE_NODE);
      attrs[i].mName.SetTo(ni);
    }
    
    rv = aElement->SetAttrAt(i, nsDependentString(aAtts[i * 2 + 1]),
                             mDocumentURI); 
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}
#endif