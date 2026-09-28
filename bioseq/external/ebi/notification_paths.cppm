/**
 * @file external/ebi/notification_paths.cppm
 * @brief OpenAPI definitions for the EBI log and mail endpoints.
 */
module;

#include <string_view>

export module bioseq.external.ebi:notification_paths;

export namespace bioseq::external::ebi::descriptor {

/** POST /log request and response definitions. */
inline constexpr std::string_view log_path = R"bioseq_openapi(  "/log":
    post:
      operationId: handleLogging
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/FrontEndLogRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/FrontEndLogResponse"
)bioseq_openapi";

/** POST /mail request and response definitions. */
inline constexpr std::string_view mail_path = R"bioseq_openapi(  "/mail":
    post:
      operationId: sendEmail
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/EmailRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/EmailResponse"
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
