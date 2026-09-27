/**
 * @file external/ncbi/biosample_paths.cppm
 * @brief OpenAPI definition for retrieving NCBI BioSample reports.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:biosample_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief BioSample report request by accession, with supported response formats. */
inline constexpr std::string_view biosample_report_path =
    R"bioseq_openapi(  /biosample/accession/{accessions}/biosample_report:
    get:
      summary: Get BioSample dataset reports by accession(s)
      description: 'Get BioSample dataset reports by accession(s). By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - BioSample
      operationId: BioSample_dataset_report
      responses:
        default:
          description: An unexpected error response.
          content:
            text/plain:
              schema:
                $ref: '#/components/schemas/rpcStatus'
        '200':
          description: A successful response
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/v2reportsBioSampleDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsBioSampleDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: accessions
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: SAMN15960293
            summary: Animal sample from Gallus gallus, bGalGal3
          example-1:
            value: SAMN12629504
            summary: Animal sample from Carcharodon carcharias
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
