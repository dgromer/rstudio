/*
 * SlideRequestHandler.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */


#include "SlideRequestHandler.hpp"

#include <iostream>

#include <boost/utility.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <boost/regex.hpp>
#include <boost/iostreams/filter/regex.hpp>

#include <core/FileSerializer.hpp>
#include <core/HtmlUtils.hpp>
#include <core/markdown/Markdown.hpp>
#include <core/text/TemplateFilter.hpp>
#include <core/system/Process.hpp>

#include <r/RExec.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/projects/SessionProjects.hpp>

#include "PresentationState.hpp"
#include "SlideParser.hpp"
#include "SlideRenderer.hpp"

using namespace core;

namespace session {
namespace modules { 
namespace presentation {

namespace {

class ResourceFiles : boost::noncopyable
{
private:
   ResourceFiles() {}

public:
   std::string get(const std::string& path)
   {
      if (cache_.find(path) == cache_.end())
         cache_[path] = module_context::resourceFileAsString(path);
      return cache_[path];
   }

private:
   friend ResourceFiles& resourceFiles();
   std::map<std::string,std::string> cache_;
};

ResourceFiles& resourceFiles()
{
   static ResourceFiles instance;
   return instance;
}


std::string revealResource(const std::string& path,
                           bool embed,
                           const std::string& extraAttribs)
{
   // determine type
   bool isCss = boost::algorithm::ends_with(path, "css");

   // generate code for link vs. embed
   std::string code;
   if (embed)
   {
      if (isCss)
      {
         code = "<style type=\"text/css\" " + extraAttribs + " >\n" +
               resourceFiles().get("presentation/" + path) + "\n"
               + "</style>";
      }
      else
      {
         code = "<script type=\"text/javascript\" " + extraAttribs + " >\n" +
               resourceFiles().get("presentation/" + path) + "\n"
               + "</script>";
      }
   }
   else
   {
      if (isCss)
      {
         code = "<link rel=\"stylesheet\" href=\"" + path + "\" "
                 + extraAttribs + " >";
      }
      else
      {
         code = "<script src=\"" + path + "\" " + extraAttribs + " ></script>";
      }
   }

   return code;
}

std::string revealEmbed(const std::string& path,
                        const std::string& extraAttribs = std::string())
{
   return revealResource(path, true, extraAttribs);
}

std::string revealLink(const std::string& path,
                       const std::string& extraAttribs = std::string())
{
   return revealResource(path, false, extraAttribs);
}


std::string remoteMathjax()
{
   return resourceFiles().get("presentation/mathjax.html");
}


std::string localMathjax()
{
   return boost::algorithm::replace_first_copy(
        remoteMathjax(),
        "https://c328740.ssl.cf1.rackcdn.com/mathjax/2.0-latest",
        "mathjax");
}

std::string localWebFonts()
{
   return "@import url('revealjs/fonts/NewsCycle.css');\n"
          "@import url('revealjs/fonts/Lato.css');";
}

std::string remoteWebFonts()
{
   return "@import url('https://fonts.googleapis.com/css?family=News+Cycle:400,700');\n"
          "@import url('https://fonts.googleapis.com/css?family=Lato:400,700,400italic,700italic');";
}

bool hasKnitrVersion1()
{
   bool hasVersion = false;
   Error error = r::exec::RFunction(".rs.hasKnitrVersion1").call(&hasVersion);
   if (error)
      LOG_ERROR(error);
   return hasVersion;
}

bool performKnit(const FilePath& rmdPath, std::string* pErrMsg)
{
   // first detect whether we even need to knit -- if there is an .md
   // file with timestamp the same as or later than the .Rmd then skip it
   FilePath mdPath = rmdPath.parent().childPath(rmdPath.stem() + ".md");
   if (mdPath.exists() && (mdPath.lastWriteTime() > rmdPath.lastWriteTime()))
      return true;

   // R binary
   FilePath rProgramPath;
   Error error = module_context::rScriptPath(&rProgramPath);
   if (error)
   {
      *pErrMsg = error.summary();
      return false;
   }

   // confirm correct version of knitr
   if (!hasKnitrVersion1())
   {
      *pErrMsg = "knitr version 1.0 or greater is required for presentations";
      return false;
   }

   // removet the target file
   error = mdPath.removeIfExists();
   if (error)
      LOG_ERROR(error);

   // args
   std::vector<std::string> args;
   args.push_back("--silent");
   args.push_back("--no-save");
   args.push_back("--no-restore");
   args.push_back("-e");
   boost::format fmt("library(knitr); "
                     "opts_chunk$set(cache=TRUE, "
                                    "cache.path='%1%-cache/', "
                                    "fig.path='%1%-figure/', "
                                    "tidy=FALSE, "
                                    "warning=FALSE, "
                                    "message=FALSE, "
                                    "comment=NA); "
                     "knit('%2%', encoding='%3%');");
   std::string encoding = projects::projectContext().defaultEncoding();
   std::string cmd = boost::str(fmt % rmdPath.stem()
                                    % rmdPath.filename()
                                    % encoding);
   args.push_back(cmd);

   // options
   core::system::ProcessOptions options;
   core::system::ProcessResult result;
   options.workingDir = rmdPath.parent();

   // run knit
   error = core::system::runProgram(
            core::string_utils::utf8ToSystem(rProgramPath.absolutePath()),
            args,
            "",
            options,
            &result);
   if (error)
   {
      *pErrMsg = error.summary();
      return false;
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      *pErrMsg = "Error occurred during knit: " + result.stdErr;
      return false;
   }
   else
   {
      return true;
   }
}

std::string fixupLink(const boost::cmatch& match)
{
   std::string href = http::util::urlDecode(match[1]);

   if (boost::algorithm::starts_with(href, "#"))
   {
      // leave internal links alone
      return match[0];
   }
   else if (href.find("://") != std::string::npos)
   {
      // open external links in a new window
      return match[0] + " target=\"_blank\"";
   }
   else if (boost::algorithm::starts_with(href, "help-topic:") ||
            boost::algorithm::starts_with(href, "help-doc:"))
   {
      // convert help commands to javascript calls
      std::string onClick;
      std::size_t colonLoc = href.find_first_of(':');
      if (href.size() > colonLoc+2)
      {
         std::ostringstream ostr;
         ostr << "onclick='";
         ostr << "window.parent.dispatchPresentationCommand(";
         json::Object cmdObj;
         using namespace boost::algorithm;
         cmdObj["name"] = trim_copy(href.substr(0, colonLoc));
         cmdObj["params"] = trim_copy(href.substr(colonLoc+1));
         json::write(cmdObj, ostr);
         ostr << "); return false;'";
         onClick = ostr.str();
      }

      return match[0] + " " + onClick;
   }
   else
   {
      return match[0];
   }
}

boost::iostreams::regex_filter linkFilter()
{
   return boost::iostreams::regex_filter(
            boost::regex("<a href=\"([^\"]+)\""),
            fixupLink);
}


bool readPresentation(SlideDeck* pSlideDeck,
                      std::string* pSlides,
                      std::string* pInitActions,
                      std::string* pSlideActions,
                      std::map<std::string,std::string>* pVars,
                      std::string* pErrMsg)
{
   // look for slides.Rmd and knit if we need to
   FilePath presDir = presentation::state::directory();
   FilePath rmdFile = presDir.complete("slides.Rmd");
   if (rmdFile.exists())
   {
      if (!performKnit(rmdFile, pErrMsg))
         return false;
   }

   // look for slides.md
   FilePath slidesFile = presDir.complete("slides.md");
   if (!slidesFile.exists())
   {
      *pErrMsg = "slides.md file not found in " +
                 presentation::state::directory().absolutePath();
      return false;
   }

   // parse the slides
   Error error = pSlideDeck->readSlides(slidesFile);
   if (error)
   {
      LOG_ERROR(error);
      *pErrMsg = error.summary();
      return false;
   }

   // render the slides
   std::string revealConfig;
   error = presentation::renderSlides(*pSlideDeck,
                                      pSlides,
                                      &revealConfig,
                                      pInitActions,
                                      pSlideActions);
   if (error)
   {
      LOG_ERROR(error);
      *pErrMsg = error.summary();
      return false;
   }

   // build template variables
   std::map<std::string,std::string>& vars = *pVars;
   vars["title"] = pSlideDeck->title();
   vars["slides"] = *pSlides;
   vars["slides_css"] =  resourceFiles().get("presentation/slides.css");
   vars["r_highlight"] = resourceFiles().get("r_highlight.html");
   vars["reveal_config"] = revealConfig;


   return true;
}

bool renderPresentation(
                   const std::map<std::string,std::string>& vars,
                   const std::vector<boost::iostreams::regex_filter>& filters,
                   std::ostream& os,
                   std::string* pErrMsg)
{
   std::string presentationTemplate =
                           resourceFiles().get("presentation/slides.html");
   std::istringstream templateStream(presentationTemplate);

   try
   {
      os.exceptions(std::istream::failbit | std::istream::badbit);
      boost::iostreams::filtering_ostream filteredStream ;

      // template filter
      text::TemplateFilter templateFilter(vars);
      filteredStream.push(templateFilter);

      // custom filters
      for (std::size_t i=0; i<filters.size(); i++)
         filteredStream.push(filters[i]);

      // target stream
      filteredStream.push(os);

      boost::iostreams::copy(templateStream, filteredStream, 128);
   }
   catch(const std::exception& e)
   {
      *pErrMsg = e.what();
      return false;
   }

   return true;
}

typedef boost::function<void(const std::string&,
                             std::map<std::string,std::string>*)> VarSource;

void publishToRPubsVars(const std::string& slides,
                        std::map<std::string,std::string>* pVars)
{
   std::map<std::string,std::string>& vars = *pVars;

   // webfonts w/ remote url
   vars["google_webfonts"] = remoteWebFonts();

   // mathjax w/ remote url
   if (markdown::isMathJaxRequired(slides))
      vars["mathjax"] = remoteMathjax();
   else
      vars["mathjax"] = "";
}

bool createStandalonePresentation(const FilePath& targetFile,
                                  const VarSource& varSource,
                                  std::string* pErrMsg)
{
   // read presentation
   presentation::SlideDeck slideDeck;
   std::string slides, initCommands, slideCommands;
   std::map<std::string,std::string> vars;
   std::string errMsg;
   if (!readPresentation(&slideDeck,
                         &slides,
                         &initCommands,
                         &slideCommands,
                         &vars,
                         pErrMsg))
   {
      return false;
   }

   // embedded versions of reveal assets
   const char * const kMediaPrint = "media=\"print\"";
   vars["reveal_print_pdf_css"] = revealEmbed("revealjs/css/print/pdf.css",
                                              kMediaPrint);
   vars["reveal_css"] = revealEmbed("revealjs/css/reveal.min.css");
   vars["reveal_theme_css"] = revealEmbed("revealjs/css/theme/simple.css");
   vars["reveal_head_js"] = revealEmbed("revealjs/lib/js/head.min.js");
   vars["reveal_js"] = revealEmbed("revealjs/js/reveal.min.js");

   // call var source hook function
   varSource(slides, &vars);

   // no IDE interaction
   vars["slide_commands"] = "";
   vars["slides_js"] = "";
   vars["init_commands"] = "";

   // width and height (these are the reveal defaults)
   vars["reveal_width"] = "960";
   vars["reveal_height"] = "700";

   // use transitions for standalone
   vars["reveal_transition"] = slideDeck.transition();

   // target file stream
   boost::shared_ptr<std::ostream> pOfs;
   Error error = targetFile.open_w(&pOfs);
   if (error)
   {
      LOG_ERROR(error);
      *pErrMsg = error.summary();
      return false;
   }

   // create image filter
   FilePath dirPath = presentation::state::directory();
   std::vector<boost::iostreams::regex_filter> filters;
   filters.push_back(html_utils::Base64ImageFilter(dirPath));

   // render presentation
   return renderPresentation(vars, filters, *pOfs, &errMsg);
}


void handlePresentationRootRequest(const std::string& path,
                                   http::Response* pResponse)
{   
   // read presentation
   presentation::SlideDeck slideDeck;
   std::string slides, initCommands, slideCommands;
   std::map<std::string,std::string> vars;
   std::string errMsg;
   if (!readPresentation(&slideDeck,
                         &slides,
                         &initCommands,
                         &slideCommands,
                         &vars,
                         &errMsg))
   {
      pResponse->setError(http::status::InternalServerError,
                          errMsg);
      return;
   }

   // set preload to none for media
   vars["slides"] = boost::algorithm::replace_all_copy(
                                          vars["slides"],
                                          "controls preload=\"auto\"",
                                          "controls preload=\"none\"");

   // linked versions of reveal assets
   vars["reveal_css"] = revealLink("revealjs/css/reveal.css");
   vars["reveal_theme_css"] = revealLink("revealjs/css/theme/simple.css");
   vars["reveal_head_js"] = revealLink("revealjs/lib/js/head.min.js");
   vars["reveal_js"] = revealLink("revealjs/js/reveal.js");

   // no print css for qtwebkit
   vars["reveal_print_pdf_css"]  = "";

   // webfonts local
   vars["google_webfonts"] = localWebFonts();

   // mathjax local
   if (markdown::isMathJaxRequired(slides))
      vars["mathjax"] = localMathjax();
   else
      vars["mathjax"] = "";

   // javascript supporting IDE interaction
   vars["slide_commands"] = slideCommands;
   vars["slides_js"] = resourceFiles().get("presentation/slides.js");
   vars["init_commands"] = initCommands;

   // width and height are dynamic
   std::string zoomStr = (path == "zoom") ? "true" : "false";
   vars["reveal_width"] = "revealDetectWidth(" + zoomStr + ")";
   vars["reveal_height"] = "revealDetectHeight(" + zoomStr + ")";

   // no transition in desktop mode (qtwebkit can't keep up)
   bool isDesktop = options().programMode() == kSessionProgramModeDesktop;
   vars["reveal_transition"] =  isDesktop? "none" : slideDeck.transition();

   // render to output stream
   std::stringstream previewOutputStream;
   std::vector<boost::iostreams::regex_filter> filters;
   filters.push_back(linkFilter());
   if (renderPresentation(vars, filters, previewOutputStream, &errMsg))
   {
      pResponse->setNoCacheHeaders();
      pResponse->setBody(previewOutputStream);
   }
   else
   {
      pResponse->setError(http::status::InternalServerError, errMsg);
   }
}




void handlePresentationHelpMarkdownRequest(const FilePath& filePath,
                                           const std::string& jsCallbacks,
                                           core::http::Response* pResponse)
{
   // indirection on the actual md file (related to processing R markdown)
   FilePath mdFilePath;

   // knit if required
   if (filePath.mimeContentType() == "text/x-r-markdown")
   {
      // actual file path will be the md file
      mdFilePath = filePath.parent().complete(filePath.stem() + ".md");

      // do the knit if we need to
      std::string errMsg;
      if (!performKnit(filePath, &errMsg))
      {
         pResponse->setError(http::status::InternalServerError,
                             errMsg);
         return;

      }
   }
   else
   {
      mdFilePath = filePath;
   }

   // read in the file (process markdown)
   std::string helpDoc;
   Error error = markdown::markdownToHTML(mdFilePath,
                                          markdown::Extensions(),
                                          markdown::HTMLOptions(),
                                          &helpDoc);
   if (error)
   {
      pResponse->setError(error);
      return;
   }

   // process the template
   std::map<std::string,std::string> vars;
   vars["title"] = html_utils::defaultTitle(helpDoc);
   vars["styles"] = resourceFiles().get("presentation/helpdoc.css");
   vars["r_highlight"] = resourceFiles().get("r_highlight.html");
   if (markdown::isMathJaxRequired(helpDoc))
      vars["mathjax"] = localMathjax();
   else
      vars["mathjax"] = "";
   vars["content"] = helpDoc;
   vars["js_callbacks"] = jsCallbacks;
   pResponse->setNoCacheHeaders();
   pResponse->setBody(resourceFiles().get("presentation/helpdoc.html"),
                      text::TemplateFilter(vars));
}

void handleRangeRequest(const FilePath& targetFile,
                        const http::Request& request,
                        http::Response* pResponse)
{
   // cache the last file
   struct RangeFileCache
   {
      FileInfo file;
      std::string contentType;
      std::string contents;

      void clear()
      {
         file = FileInfo();
         contentType.clear();
         contents.clear();
      }
   };
   static RangeFileCache s_cache;

   // see if we need to do a fresh read
   if (targetFile.absolutePath() != s_cache.file.absolutePath() ||
       targetFile.lastWriteTime() != s_cache.file.lastWriteTime())
   {
      // clear the cache
      s_cache.clear();

      // read the file in from disk
      Error error = core::readStringFromFile(targetFile, &(s_cache.contents));
      if (error)
      {
         pResponse->setError(error);
         return;
      }

      // update the cache
      s_cache.file = FileInfo(targetFile);
      s_cache.contentType = targetFile.mimeContentType();
   }

   // always serve from the cache
   pResponse->setRangeableFile(s_cache.contents,
                               s_cache.contentType,
                               request);


}

} // anonymous namespace


void handlePresentationPaneRequest(const http::Request& request,
                                   http::Response* pResponse)
{
   // return not found if presentation isn't active
   if (!presentation::state::isActive())
   {
      pResponse->setError(http::status::NotFound, request.uri() + " not found");
      return;
   }

   // get the requested path
   std::string path = http::util::pathAfterPrefix(request, "/presentation/");

   // special handling for the root
   if (path.empty() || (path == "zoom"))
   {
      handlePresentationRootRequest(path, pResponse);
   }

   // special handling for reveal.js assets
   else if (boost::algorithm::starts_with(path, "revealjs/"))
   {
      path = http::util::pathAfterPrefix(request, "/presentation/revealjs/");
      FilePath resPath = options().rResourcesPath().complete("presentation");
      FilePath filePath = resPath.complete("revealjs/" + path);
      pResponse->setFile(filePath, request);
   }

   // special handling for mathjax assets
   else if (boost::algorithm::starts_with(path, "mathjax/"))
   {
      FilePath filePath =
            session::options().mathjaxPath().parent().childPath(path);
      pResponse->setFile(filePath, request);
   }


   // serve the file back
   else
   {
      FilePath targetFile = presentation::state::directory().childPath(path);
      if (!request.headerValue("Range").empty())
      {
         handleRangeRequest(targetFile, request, pResponse);
      }
      else
      {
         // indicate that we accept byte range requests
         pResponse->addHeader("Accept-Ranges", "bytes");

         // return the file
         pResponse->setFile(targetFile, request);
      }
   }
}

void handlePresentationHelpRequest(const core::http::Request& request,
                                   const std::string& jsCallbacks,
                                   core::http::Response* pResponse)
{
   // we save the most recent /help/presentation/&file=parameter so we
   // can resolve relative file references against it. we do this
   // separately from presentation::state::directory so that the help
   // urls can be available within the help pane (and history)
   // independent of the duration of the presentation tab
   static FilePath s_presentationHelpDir;

   // check if this is a root request
   std::string file = request.queryParamValue("file");
   if (!file.empty())
   {
      // ensure file exists
      FilePath filePath = module_context::resolveAliasedPath(file);
      if (!filePath.exists())
      {
         pResponse->setError(http::status::NotFound, request.uri());
         return;
      }

      // save the help dir
      s_presentationHelpDir = filePath.parent();

      // check for markdown
      if (filePath.mimeContentType() == "text/x-markdown" ||
          filePath.mimeContentType() == "text/x-r-markdown")
      {
         handlePresentationHelpMarkdownRequest(filePath,
                                               jsCallbacks,
                                               pResponse);
      }

      // just a stock file
      else
      {
         pResponse->setFile(filePath, request);
      }
   }

   // it's a relative file reference
   else
   {
      // make sure the directory exists
      if (!s_presentationHelpDir.exists())
      {
         pResponse->setError(http::status::NotFound,
                             "Directory not found: " +
                             s_presentationHelpDir.absolutePath());
         return;
      }

      // resolve the file reference
      std::string path = http::util::pathAfterPrefix(request,
                                                     "/help/presentation/");

      // serve the file back
      pResponse->setFile(s_presentationHelpDir.complete(path), request);
   }
}


SEXP rs_createStandalonePresentation()
{
   FilePath dirPath = presentation::state::directory();
   FilePath htmlPath = dirPath.complete(dirPath.stem() + ".html");

   std::string errMsg;
   if (!createStandalonePresentation(htmlPath, publishToRPubsVars, &errMsg))
   {
      module_context::consoleWriteError(errMsg + "\n");
   }

   return R_NilValue;
}



} // namespace presentation
} // namespace modules
} // namesapce session
